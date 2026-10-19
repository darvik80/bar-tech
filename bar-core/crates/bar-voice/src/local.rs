use crate::wav::{pcm16_to_wav, resample_i16_to_16k};
use crate::{SttEngine, TtsEngine};
use anyhow::{anyhow, Context, Result};
use async_trait::async_trait;
use bar_config::{SttLocalConfig, TtsLocalConfig};
use reqwest::Client;
use std::path::PathBuf;
use std::process::Stdio;
use tokio::process::Command;
use tracing::{info, warn};

pub struct LocalStt {
    http: Client,
    base_url: String,
    language: String,
}

impl LocalStt {
    pub fn new(cfg: &SttLocalConfig) -> Result<Self> {
        Ok(Self {
            http: Client::new(),
            base_url: cfg.base_url.trim_end_matches('/').to_string(),
            language: cfg.language.clone(),
        })
    }
}

#[async_trait]
impl SttEngine for LocalStt {
    async fn transcribe(&self, pcm_16k_mono: &[u8]) -> Result<String> {
        let wav = pcm16_to_wav(pcm_16k_mono, 16000);
        let part = reqwest::multipart::Part::bytes(wav)
            .file_name("audio.wav")
            .mime_str("audio/wav")?;
        let form = reqwest::multipart::Form::new()
            .part("file", part)
            .text("language", self.language.clone())
            .text("response_format", "json");
        let url = format!("{}/inference", self.base_url);
        let resp = self.http.post(&url).multipart(form).send().await?;
        let status = resp.status();
        let body = resp.text().await?;
        if !status.is_success() {
            return Err(anyhow!("local stt {status}: {body}"));
        }
        // whisper.cpp server returns {"text":"..."} or plain text
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&body) {
            if let Some(t) = v.get("text").and_then(|x| x.as_str()) {
                return Ok(t.trim().to_string());
            }
        }
        Ok(body.trim().to_string())
    }
}

pub struct LocalTts {
    binary: PathBuf,
    model_path: PathBuf,
}

impl LocalTts {
    pub fn new(cfg: &TtsLocalConfig) -> Result<Self> {
        let model = PathBuf::from(&cfg.model_dir).join(format!("{}.onnx", cfg.voice));
        Ok(Self {
            binary: PathBuf::from(&cfg.binary_path),
            model_path: model,
        })
    }
}

#[async_trait]
impl TtsEngine for LocalTts {
    async fn synthesize(&self, text: &str) -> Result<Vec<u8>> {
        if !self.binary.exists() {
            warn!(path = %self.binary.display(), "piper binary missing — returning silence");
            return Ok(vec![0u8; 16000 * 2]); // 1s silence
        }
        let dir = tempfile::tempdir()?;
        let in_txt = dir.path().join("in.txt");
        let out_wav = dir.path().join("out.wav");
        tokio::fs::write(&in_txt, text).await?;
        let status = Command::new(&self.binary)
            .arg("--model")
            .arg(&self.model_path)
            .arg("--output_file")
            .arg(&out_wav)
            .stdin(Stdio::null())
            .arg("--")
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .arg(text)
            .status()
            .await;
        // Piper CLI variants differ; try file-based invoke
        let ok = match status {
            Ok(s) if s.success() && out_wav.exists() => true,
            _ => {
                let mut child = Command::new(&self.binary)
                    .arg("--model")
                    .arg(&self.model_path)
                    .arg("--output_file")
                    .arg(&out_wav)
                    .stdin(Stdio::piped())
                    .stdout(Stdio::null())
                    .stderr(Stdio::piped())
                    .spawn()
                    .context("spawn piper")?;
                if let Some(mut stdin) = child.stdin.take() {
                    use tokio::io::AsyncWriteExt;
                    stdin.write_all(text.as_bytes()).await?;
                }
                let s = child.wait().await?;
                s.success() && out_wav.exists()
            }
        };
        if !ok {
            return Err(anyhow!("piper failed"));
        }
        let wav = tokio::fs::read(&out_wav).await?;
        let pcm = parse_wav_to_i16(&wav)?;
        let rate = wav_sample_rate(&wav).unwrap_or(22050);
        info!(src_rate = rate, "piper synth ok");
        Ok(resample_i16_to_16k(&pcm, rate))
    }
}

fn wav_sample_rate(wav: &[u8]) -> Option<u32> {
    if wav.len() < 28 {
        return None;
    }
    Some(u32::from_le_bytes([wav[24], wav[25], wav[26], wav[27]]))
}

fn parse_wav_to_i16(wav: &[u8]) -> Result<Vec<i16>> {
    let data_pos = wav
        .windows(4)
        .position(|w| w == b"data")
        .ok_or_else(|| anyhow!("no data chunk"))?;
    let start = data_pos + 8;
    if start >= wav.len() {
        return Err(anyhow!("bad wav"));
    }
    let pcm = &wav[start..];
    Ok(pcm
        .chunks_exact(2)
        .map(|c| i16::from_le_bytes([c[0], c[1]]))
        .collect())
}
