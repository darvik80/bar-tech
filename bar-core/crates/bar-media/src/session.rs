use crate::opus::OpusCodec;
use std::net::SocketAddr;
use std::time::Instant;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    Idle,
    Opening,
    Streaming,
    Closing,
    Closed,
}

pub struct AudioSession {
    pub session_id: String,
    pub udp_session_id: u32,
    pub device_id: String,
    pub state: SessionState,
    pub created_at: Instant,
    pub last_activity: Instant,
    pub device_addr: Option<SocketAddr>,
    pub opus: OpusCodec,
    pub pcm_buffer: Vec<u8>,
    pub speech_started: bool,
    pub speech_ended: bool,
    pub callback_called: bool,
    pub last_seq: i32,
    pub expected_seq: u16,
    pub out_seq: u16,
}

impl AudioSession {
    pub fn new(session_id: String, udp_session_id: u32, device_id: String) -> anyhow::Result<Self> {
        Ok(Self {
            session_id,
            udp_session_id,
            device_id,
            state: SessionState::Opening,
            created_at: Instant::now(),
            last_activity: Instant::now(),
            device_addr: None,
            opus: OpusCodec::new()?,
            pcm_buffer: Vec::new(),
            speech_started: false,
            speech_ended: false,
            callback_called: false,
            last_seq: -1,
            expected_seq: 0,
            out_seq: 0,
        })
    }

    pub fn touch(&mut self) {
        self.last_activity = Instant::now();
    }

    pub fn is_timed_out(&self, timeout_secs: u64) -> bool {
        self.last_activity.elapsed().as_secs() > timeout_secs
    }

    pub fn validate_sequence(&mut self, seq: u16) -> bool {
        if self.last_seq < 0 {
            self.expected_seq = seq;
            self.last_seq = seq as i32;
            return true;
        }
        if seq < self.expected_seq && self.expected_seq - seq < 1000 {
            return false;
        }
        self.expected_seq = seq.wrapping_add(1);
        self.last_seq = seq as i32;
        true
    }
}
