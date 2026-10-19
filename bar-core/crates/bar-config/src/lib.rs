use anyhow::{Context, Result};
use serde::Deserialize;
use std::env;
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Deserialize)]
pub struct AppConfig {
    pub http: HttpConfig,
    pub mqtt: MqttConfig,
    pub audio: AudioConfig,
    pub stt: SttConfig,
    pub tts: TtsConfig,
    pub llm: LlmConfig,
    pub rag: RagConfig,
    pub db: DbConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct HttpConfig {
    pub bind: String,
    pub port: u16,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MqttConfig {
    pub broker: String,
    pub plugin_id: String,
    pub product_id: String,
    pub username: String,
    pub password: String,
    /// e.g. `platform/v1/events/def/{product_id}/#`
    pub events_subscribe: String,
    /// e.g. `platform/v1/commands/{product_id}`
    pub commands_prefix: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AudioConfig {
    pub udp_bind: String,
    pub udp_port: u16,
    pub advertise_ip: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum VoiceMode {
    Local,
    Cloud,
}

#[derive(Debug, Clone, Deserialize)]
pub struct SttConfig {
    pub mode: VoiceMode,
    pub local: SttLocalConfig,
    pub cloud: CloudVoiceConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct SttLocalConfig {
    pub base_url: String,
    pub language: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct CloudVoiceConfig {
    pub base_url: String,
    pub api_key_env: String,
    pub model: String,
    #[serde(default)]
    pub voice: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct TtsConfig {
    pub mode: VoiceMode,
    pub local: TtsLocalConfig,
    pub cloud: CloudVoiceConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct TtsLocalConfig {
    pub binary_path: String,
    pub model_dir: String,
    pub voice: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LlmConfig {
    pub base_url: String,
    pub api_key_env: String,
    pub model: String,
    pub temperature: f32,
    pub max_tokens: u32,
    pub timeout_sec: u64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct RagConfig {
    pub knowledge_dir: String,
    pub top_k: usize,
    pub embed_model: String,
    pub chunk_size: usize,
    pub chunk_overlap: usize,
}

#[derive(Debug, Clone, Deserialize)]
pub struct DbConfig {
    pub path: String,
    pub max_turns: usize,
    pub retention_days: u32,
}

impl AppConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let raw = fs::read_to_string(path)
            .with_context(|| format!("read config {}", path.display()))?;
        let cfg: AppConfig = serde_yaml::from_str(&raw).context("parse config yaml")?;
        Ok(cfg)
    }

    pub fn resolve_api_key(env_name: &str) -> Option<String> {
        env::var(env_name).ok().filter(|s| !s.is_empty())
    }
}
