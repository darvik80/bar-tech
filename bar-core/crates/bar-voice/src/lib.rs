mod cloud;
mod local;
mod wav;

use anyhow::Result;
use async_trait::async_trait;
use bar_config::{SttConfig, TtsConfig, VoiceMode};
use cloud::{CloudStt, CloudTts};
use local::{LocalStt, LocalTts};

#[async_trait]
pub trait SttEngine: Send + Sync {
    async fn transcribe(&self, pcm_16k_mono: &[u8]) -> Result<String>;
}

#[async_trait]
pub trait TtsEngine: Send + Sync {
    async fn synthesize(&self, text: &str) -> Result<Vec<u8>>;
}

pub fn build_stt(cfg: &SttConfig) -> Result<Box<dyn SttEngine>> {
    match cfg.mode {
        VoiceMode::Local => Ok(Box::new(LocalStt::new(&cfg.local)?)),
        VoiceMode::Cloud => Ok(Box::new(CloudStt::new(&cfg.cloud)?)),
    }
}

pub fn build_tts(cfg: &TtsConfig) -> Result<Box<dyn TtsEngine>> {
    match cfg.mode {
        VoiceMode::Local => Ok(Box::new(LocalTts::new(&cfg.local)?)),
        VoiceMode::Cloud => Ok(Box::new(CloudTts::new(&cfg.cloud)?)),
    }
}

pub use wav::pcm16_to_wav;
