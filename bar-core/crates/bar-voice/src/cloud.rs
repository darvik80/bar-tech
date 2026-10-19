use crate::wav::{pcm16_to_wav, resample_i16_to_16k};
use crate::{SttEngine, TtsEngine};
use anyhow::{anyhow, Context, Result};
use async_trait::async_trait;
use bar_config::CloudVoiceConfig;
use reqwest::Client;
use serde_json::json;
use tracing::debug;

pub struct CloudStt {
    http: Client,
    base_url: String,
    api_key: Option<String>,
    model: String,
}

impl CloudStt {
    pub fn new(cfg: &CloudVoiceConfig) -> Result<Self> {
        Ok(Self {
            http: Client::new(),
            base_url: cfg.base_url.trim_end_matches('/').to_string(),
            api_key: bar_config::AppConfig::resolve_api_key(&cfg.api_key_env),
            model: cfg.model.clone(),
        })
    }
}

#[async_trait]
impl SttEngine for CloudStt {
    async fn transcribe(&self, pcm_16k_mono: &[u8]) -> Result<String> {
        let wav = pcm16_to_wav(pcm_16k_mono, 16000);
        let part = reqwest::multipart::Part::bytes(wav)
            .file_name("audio.wav")
            .mime_str("audio/wav")?;
        let form = reqwest::multipart::Form::new()
            .part("file", part)
            .text("model", self.model.clone());
        let mut req = self
            .http
            .post(format!("{}/audio/transcriptions", self.base_url))
            .multipart(form);
        if let Some(key) = &self.api_key {
            req = req.bearer_auth(key);
        }
        let resp = req.send().await.context("cloud stt")?;
        let status = resp.status();
        let body = resp.text().await?;
        if !status.is_success() {
            return Err(anyhow!("cloud stt {status}: {body}"));
        }
        let v: serde_json::Value = serde_json::from_str(&body)?;
        Ok(v.get("text")
            .and_then(|t| t.as_str())
            .unwrap_or("")
            .trim()
            .to_string())
    }
}

pub struct CloudTts {
    http: Client,
    base_url: String,
    api_key: Option<String>,
    model: String,
    voice: String,
}

impl CloudTts {
    pub fn new(cfg: &CloudVoiceConfig) -> Result<Self> {
        Ok(Self {
            http: Client::new(),
            base_url: cfg.base_url.trim_end_matches('/').to_string(),
            api_key: bar_config::AppConfig::resolve_api_key(&cfg.api_key_env),
            model: cfg.model.clone(),
            voice: cfg.voice.clone().unwrap_or_else(|| "alloy".into()),
        })
    }
}

#[async_trait]
impl TtsEngine for CloudTts {
    async fn synthesize(&self, text: &str) -> Result<Vec<u8>> {
        let body = json!({
            "model": self.model,
            "voice": self.voice,
            "input": text,
            "response_format": "wav",
        });
        let mut req = self
            .http
            .post(format!("{}/audio/speech", self.base_url))
            .json(&body);
        if let Some(key) = &self.api_key {
            req = req.bearer_auth(key);
        }
        debug!("cloud tts request");
        let resp = req.send().await.context("cloud tts")?;
        let status = resp.status();
        let bytes = resp.bytes().await?;
        if !status.is_success() {
            return Err(anyhow!(
                "cloud tts {status}: {}",
                String::from_utf8_lossy(&bytes)
            ));
        }
        // Assume 24k wav from OpenAI; resample to 16k
        let rate = u32::from_le_bytes([
            bytes.get(24).copied().unwrap_or(0),
            bytes.get(25).copied().unwrap_or(0),
            bytes.get(26).copied().unwrap_or(0),
            bytes.get(27).copied().unwrap_or(0),
        ]);
        let data_pos = bytes
            .windows(4)
            .position(|w| w == b"data")
            .unwrap_or(36);
        let pcm = &bytes[data_pos + 8..];
        let samples: Vec<i16> = pcm
            .chunks_exact(2)
            .map(|c| i16::from_le_bytes([c[0], c[1]]))
            .collect();
        let rate = if rate == 0 { 24000 } else { rate };
        Ok(resample_i16_to_16k(&samples, rate))
    }
}
