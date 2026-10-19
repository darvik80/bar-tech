mod pour;

use anyhow::{Context, Result};
use bar_api::{router, ApiState};
use bar_config::AppConfig;
use bar_dialog::{DialogEngine, PourExecutor};
use bar_llm::LlmClient;
use bar_media::{SpeechComplete, UdpServer};
use bar_mqtt::{device_id_from_topic, MqttEvent, MqttPlugin, SharedMqtt};
use bar_rag::RagIndex;
use bar_storage::Database;
use bar_voice::{build_stt, build_tts, SttEngine, TtsEngine};
use pour::MqttPourExecutor;
use serde_json::json;
use std::path::PathBuf;
use std::sync::Arc;
use tracing::{error, info, warn};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,bar_core=debug".into()),
        )
        .init();

    let config_path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("config.yaml"));
    let cfg = AppConfig::load(&config_path).with_context(|| {
        format!(
            "load config from {} (copy config.example.yaml)",
            config_path.display()
        )
    })?;

    let db = Database::open(cfg.db.clone())?;
    db.purge_old()?;

    let llm = LlmClient::new(&cfg.llm, &cfg.rag.embed_model)?;
    let rag = Arc::new(RagIndex::new(cfg.rag.clone(), db.clone(), llm.clone()));
    // Best-effort index; works without API key as empty
    match rag.reindex().await {
        Ok(n) => info!(chunks = n, "rag ready"),
        Err(e) => warn!(error = %e, "rag reindex skipped"),
    }

    let (mqtt, mut mqtt_rx) = MqttPlugin::connect(cfg.mqtt.clone()).await?;
    let mqtt: SharedMqtt = Arc::new(mqtt);

    let (udp, mut speech_rx) = UdpServer::bind(cfg.audio.clone()).await?;
    let udp = Arc::new(udp);

    let stt: Arc<dyn SttEngine> = Arc::from(build_stt(&cfg.stt)?);
    let tts: Arc<dyn TtsEngine> = Arc::from(build_tts(&cfg.tts)?);

    let pour: Arc<dyn PourExecutor> = Arc::new(MqttPourExecutor::new(mqtt.clone()));
    let dialog = Arc::new(DialogEngine::new(
        db.clone(),
        llm.clone(),
        rag.clone(),
        pour,
    ));

    let api_state = ApiState {
        db: db.clone(),
        rag: rag.clone(),
    };
    let app = router(api_state);
    let bind = format!("{}:{}", cfg.http.bind, cfg.http.port);
    let listener = tokio::net::TcpListener::bind(&bind).await?;
    info!(%bind, "http listening");
    tokio::spawn(async move {
        if let Err(e) = axum::serve(listener, app).await {
            error!(error = %e, "http server failed");
        }
    });

    let mqtt_c = mqtt.clone();
    let udp_c = udp.clone();
    tokio::spawn(async move {
        while let Some(ev) = mqtt_rx.recv().await {
            if let Err(e) = handle_mqtt(&mqtt_c, &udp_c, ev).await {
                warn!(error = %e, "mqtt handler error");
            }
        }
    });

    let mqtt_s = mqtt.clone();
    let udp_s = udp.clone();
    let dialog_s = dialog.clone();
    let stt_s = stt.clone();
    let tts_s = tts.clone();
    tokio::spawn(async move {
        while let Some(speech) = speech_rx.recv().await {
            if let Err(e) =
                process_speech(&mqtt_s, &udp_s, &dialog_s, &stt_s, &tts_s, speech).await
            {
                error!(error = %e, "speech pipeline error");
            }
        }
    });

    info!("bar-core started");
    tokio::signal::ctrl_c().await?;
    info!("shutting down");
    Ok(())
}

async fn handle_mqtt(mqtt: &SharedMqtt, udp: &Arc<UdpServer>, ev: MqttEvent) -> Result<()> {
    let topic = ev.topic;
    let payload = ev.payload;

    if topic.ends_with("/status") {
        if let Some(device_id) = device_id_from_topic(&topic) {
            let online = payload.get("online").and_then(|v| v.as_bool()).unwrap_or(true);
            if !online {
                warn!(%device_id, "device offline — drop sessions");
                udp.remove_sessions_by_device_id(&device_id).await;
            }
        }
        return Ok(());
    }

    if !topic.ends_with("/event") {
        return Ok(());
    }

    let Some(device_id) = device_id_from_topic(&topic) else {
        warn!(%topic, "cannot parse device_id");
        return Ok(());
    };

    let event_type = payload
        .get("type")
        .and_then(|v| v.as_str())
        .unwrap_or("");

    match event_type {
        "wake_word" => {
            info!(%device_id, "wake_word — starting session");
            start_voice_session(mqtt, udp, &device_id).await?;
        }
        "session_closed" => {
            if let Some(sid) = payload.get("session_id").and_then(|v| v.as_str()) {
                info!(%device_id, session_id = sid, "session_closed");
                udp.remove_session_by_session_id(sid).await;
            }
        }
        other => {
            info!(%device_id, event = other, "unhandled event");
        }
    }
    Ok(())
}

async fn start_voice_session(
    mqtt: &SharedMqtt,
    udp: &Arc<UdpServer>,
    device_id: &str,
) -> Result<()> {
    let session = udp.create_session(device_id).await?;
    mqtt.publish_attributes(
        device_id,
        json!({
            "session_id": session.session_id,
            "session_status": "open",
        }),
    )
    .await?;
    mqtt.publish_command(
        device_id,
        "cogitor.session.control",
        json!({
            "action": "start",
            "session_id": session.session_id,
            "udp_session_id": session.udp_session_id,
            "server_ip": udp.advertise_ip(),
            "udp_port": udp.port(),
            "opus": {
                "sample_rate": 16000,
                "channels": 1,
                "bitrate": "auto",
                "frame_ms": 60
            }
        }),
    )
    .await?;
    info!(
        %device_id,
        session = %session.session_id,
        server = %format!("{}:{}", udp.advertise_ip(), udp.port()),
        "session.control start sent"
    );
    Ok(())
}

async fn process_speech(
    mqtt: &SharedMqtt,
    udp: &Arc<UdpServer>,
    dialog: &Arc<DialogEngine>,
    stt: &Arc<dyn SttEngine>,
    tts: &Arc<dyn TtsEngine>,
    speech: SpeechComplete,
) -> Result<()> {
    let device_id = speech.device_id.clone();
    let udp_session_id = speech.udp_session_id;

    let keepalive = {
        let udp = udp.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let _ = udp.send_keepalive(udp_session_id).await;
            }
        })
    };

    let result = async {
        mqtt.publish_attributes(
            &device_id,
            json!({"ui_state": "thinking", "ui_text": "Thinking..."}),
        )
        .await?;

        let text = if speech.pcm.len() < 16000 {
            String::new()
        } else if rms_i16(&speech.pcm) < 100.0 {
            warn!("audio too quiet");
            String::new()
        } else {
            stt.transcribe(&speech.pcm).await.unwrap_or_default()
        };

        let reply = if text.trim().is_empty() {
            "Не расслышал, повтори пожалуйста".to_string()
        } else {
            info!(%device_id, %text, "stt ok");
            dialog.process_utterance(&device_id, &text).await?
        };

        mqtt.publish_attributes(
            &device_id,
            json!({"ui_state": "speaking", "ui_text": reply}),
        )
        .await?;

        let pcm = tts.synthesize(&reply).await?;
        udp.send_pcm_response(udp_session_id, &pcm).await?;

        mqtt.publish_attributes(&device_id, json!({"ui_state": "idle", "ui_text": ""}))
            .await?;
        Ok::<(), anyhow::Error>(())
    }
    .await;

    keepalive.abort();
    if let Err(e) = &result {
        let _ = mqtt
            .publish_attributes(
                &device_id,
                json!({"ui_state": "idle", "ui_text": "Error occurred"}),
            )
            .await;
        return Err(anyhow::anyhow!(e.to_string()));
    }
    Ok(())
}

fn rms_i16(pcm: &[u8]) -> f32 {
    if pcm.len() < 2 {
        return 0.0;
    }
    let mut sum = 0.0f64;
    let mut n = 0usize;
    for c in pcm.chunks_exact(2) {
        let s = i16::from_le_bytes([c[0], c[1]]) as f64;
        sum += s * s;
        n += 1;
    }
    if n == 0 {
        0.0
    } else {
        (sum / n as f64).sqrt() as f32
    }
}
