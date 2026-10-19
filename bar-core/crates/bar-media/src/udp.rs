use crate::session::{AudioSession, SessionState};
use anyhow::{anyhow, Result};
use bar_config::AudioConfig;
use rand::Rng;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::net::UdpSocket;
use tokio::sync::{mpsc, Mutex};
use tracing::{info, warn};
use uuid::Uuid;

pub const HEADER_SIZE: usize = 8;
pub const FLAG_START_OF_SPEECH: u8 = 0x01;
pub const FLAG_END_OF_SPEECH: u8 = 0x02;
pub const FLAG_START_OF_RESPONSE: u8 = 0x01;
pub const FLAG_END_OF_RESPONSE: u8 = 0x02;

const MAX_CONCURRENT: usize = 3;
const MAX_PACKET: usize = 2048;
const MIN_AUDIO_BYTES: usize = 8000;
const SESSION_TIMEOUT_SECS: u64 = 120;

#[derive(Clone)]
pub struct SpeechComplete {
    pub udp_session_id: u32,
    pub session_id: String,
    pub device_id: String,
    pub pcm: Vec<u8>,
}

pub struct UdpServer {
    cfg: AudioConfig,
    sock: Arc<UdpSocket>,
    sessions: Arc<Mutex<HashMap<u32, AudioSession>>>,
}

impl UdpServer {
    pub async fn bind(cfg: AudioConfig) -> Result<(Self, mpsc::Receiver<SpeechComplete>)> {
        let sock = UdpSocket::bind(format!("{}:{}", cfg.udp_bind, cfg.udp_port)).await?;
        info!(addr = %sock.local_addr()?, "udp audio listening");
        let sock = Arc::new(sock);
        let sessions = Arc::new(Mutex::new(HashMap::new()));
        let (tx, rx) = mpsc::channel(16);

        let sock_c = sock.clone();
        let sessions_c = sessions.clone();
        tokio::spawn(async move {
            let mut buf = vec![0u8; MAX_PACKET];
            loop {
                match sock_c.recv_from(&mut buf).await {
                    Ok((n, addr)) => {
                        if let Err(e) =
                            handle_packet(&sessions_c, &tx, &buf[..n], addr).await
                        {
                            warn!(error = %e, "udp packet error");
                        }
                    }
                    Err(e) => warn!(error = %e, "udp recv error"),
                }
            }
        });

        let sessions_cleanup = sessions.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                let mut guard = sessions_cleanup.lock().await;
                let stale: Vec<u32> = guard
                    .iter()
                    .filter(|(_, s)| s.is_timed_out(SESSION_TIMEOUT_SECS))
                    .map(|(k, _)| *k)
                    .collect();
                for id in stale {
                    warn!(udp_session_id = id, "session timed out");
                    guard.remove(&id);
                }
            }
        });

        Ok((
            Self {
                cfg,
                sock,
                sessions,
            },
            rx,
        ))
    }

    pub fn port(&self) -> u16 {
        self.cfg.udp_port
    }

    pub fn advertise_ip(&self) -> &str {
        &self.cfg.advertise_ip
    }

    pub async fn create_session(&self, device_id: &str) -> Result<SessionInfo> {
        let mut guard = self.sessions.lock().await;
        if guard.len() >= MAX_CONCURRENT {
            return Err(anyhow!("max concurrent audio sessions reached"));
        }
        let session_id = format!("sess_{}", &Uuid::new_v4().simple().to_string()[..8]);
        let udp_session_id: u32 = rand::thread_rng().gen();
        let session = AudioSession::new(session_id.clone(), udp_session_id, device_id.to_string())?;
        guard.insert(udp_session_id, session);
        info!(%session_id, udp_session_id, %device_id, "session created");
        Ok(SessionInfo {
            session_id,
            udp_session_id,
            device_id: device_id.to_string(),
        })
    }

    pub async fn remove_session_by_session_id(&self, session_id: &str) {
        let mut guard = self.sessions.lock().await;
        if let Some((k, _)) = guard.iter().find(|(_, s)| s.session_id == session_id) {
            let k = *k;
            guard.remove(&k);
            info!(%session_id, "session removed");
        }
    }

    pub async fn remove_sessions_by_device_id(&self, device_id: &str) {
        let mut guard = self.sessions.lock().await;
        let keys: Vec<u32> = guard
            .iter()
            .filter(|(_, s)| s.device_id == device_id)
            .map(|(k, _)| *k)
            .collect();
        for k in keys {
            guard.remove(&k);
        }
    }

    pub async fn send_pcm_response(&self, udp_session_id: u32, pcm: &[u8]) -> Result<()> {
        let mut guard = self.sessions.lock().await;
        let session = guard
            .get_mut(&udp_session_id)
            .ok_or_else(|| anyhow!("session not found"))?;
        let addr = session
            .device_addr
            .ok_or_else(|| anyhow!("device addr unknown"))?;
        let frames = session.opus.encode_pcm_bytes(pcm)?;
        let total = frames.len();
        for (i, frame) in frames.into_iter().enumerate() {
            let mut flags = 0u8;
            if i == 0 {
                flags |= FLAG_START_OF_RESPONSE;
            }
            if i + 1 == total {
                flags |= FLAG_END_OF_RESPONSE;
            }
            let seq = session.out_seq;
            session.out_seq = session.out_seq.wrapping_add(1);
            let packet = pack_header(session.udp_session_id, seq, flags, &frame);
            self.sock.send_to(&packet, addr).await?;
        }
        Ok(())
    }

    pub async fn send_keepalive(&self, udp_session_id: u32) -> Result<()> {
        let guard = self.sessions.lock().await;
        let session = guard
            .get(&udp_session_id)
            .ok_or_else(|| anyhow!("session not found"))?;
        let addr = session
            .device_addr
            .ok_or_else(|| anyhow!("device addr unknown"))?;
        let packet = pack_header(session.udp_session_id, 0, 0, &[]);
        self.sock.send_to(&packet, addr).await?;
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct SessionInfo {
    pub session_id: String,
    pub udp_session_id: u32,
    pub device_id: String,
}

async fn handle_packet(
    sessions: &Arc<Mutex<HashMap<u32, AudioSession>>>,
    tx: &mpsc::Sender<SpeechComplete>,
    data: &[u8],
    addr: SocketAddr,
) -> Result<()> {
    if data.len() < HEADER_SIZE {
        return Ok(());
    }
    let (udp_session_id, frame_seq, flags) = unpack_header(data);
    let payload = &data[HEADER_SIZE..];

    let mut guard = sessions.lock().await;
    let Some(session) = guard.get_mut(&udp_session_id) else {
        warn!(udp_session_id, %addr, "unknown udp session");
        return Ok(());
    };
    if !session.validate_sequence(frame_seq) {
        return Ok(());
    }
    if session.device_addr.is_none() {
        session.device_addr = Some(addr);
        session.state = SessionState::Streaming;
        info!(session = %session.session_id, %addr, "first udp packet");
    }
    session.touch();

    if flags & FLAG_START_OF_SPEECH != 0 {
        session.speech_started = true;
    }
    if flags & FLAG_END_OF_SPEECH != 0 {
        session.speech_ended = true;
    }

    if !payload.is_empty() {
        match session.opus.decode(payload) {
            Ok(pcm) => {
                for s in pcm {
                    session.pcm_buffer.extend_from_slice(&s.to_le_bytes());
                }
            }
            Err(e) => warn!(error = %e, "opus decode failed"),
        }
    }

    if session.speech_ended && !session.callback_called {
        if session.pcm_buffer.len() < MIN_AUDIO_BYTES {
            warn!(
                session = %session.session_id,
                bytes = session.pcm_buffer.len(),
                "speech too short, skip"
            );
            session.callback_called = true;
            session.speech_ended = false;
            session.speech_started = false;
            session.callback_called = false;
            session.pcm_buffer.clear();
            return Ok(());
        }
        session.callback_called = true;
        let evt = SpeechComplete {
            udp_session_id,
            session_id: session.session_id.clone(),
            device_id: session.device_id.clone(),
            pcm: std::mem::take(&mut session.pcm_buffer),
        };
        session.speech_ended = false;
        session.callback_called = false;
        session.speech_started = false;
        session.created_at = std::time::Instant::now();
        drop(guard);
        let _ = tx.send(evt).await;
    }
    Ok(())
}

fn pack_header(session_id: u32, seq: u16, flags: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER_SIZE + payload.len());
    out.extend_from_slice(&session_id.to_be_bytes());
    out.extend_from_slice(&seq.to_be_bytes());
    out.push(flags);
    out.push(0);
    out.extend_from_slice(payload);
    out
}

fn unpack_header(data: &[u8]) -> (u32, u16, u8) {
    let session_id = u32::from_be_bytes([data[0], data[1], data[2], data[3]]);
    let seq = u16::from_be_bytes([data[4], data[5]]);
    let flags = data[6];
    (session_id, seq, flags)
}
