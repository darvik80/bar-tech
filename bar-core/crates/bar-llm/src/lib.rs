use anyhow::{anyhow, Context, Result};
use bar_config::LlmConfig;
use reqwest::Client;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::time::Duration;
use tracing::debug;

#[derive(Clone)]
pub struct LlmClient {
    http: Client,
    base_url: String,
    api_key: Option<String>,
    model: String,
    temperature: f32,
    max_tokens: u32,
    embed_model: String,
}

impl LlmClient {
    pub fn new(cfg: &LlmConfig, embed_model: &str) -> Result<Self> {
        let api_key = bar_config::AppConfig::resolve_api_key(&cfg.api_key_env);
        let http = Client::builder()
            .timeout(Duration::from_secs(cfg.timeout_sec))
            .build()?;
        Ok(Self {
            http,
            base_url: cfg.base_url.trim_end_matches('/').to_string(),
            api_key,
            model: cfg.model.clone(),
            temperature: cfg.temperature,
            max_tokens: cfg.max_tokens,
            embed_model: embed_model.to_string(),
        })
    }

    pub async fn chat_completion(
        &self,
        messages: &[Value],
        tools: Option<&[Value]>,
    ) -> Result<ChatResponse> {
        let mut body = json!({
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
        });
        if let Some(tools) = tools {
            if !tools.is_empty() {
                body["tools"] = Value::Array(tools.to_vec());
                body["tool_choice"] = json!("auto");
            }
        }
        let mut req = self
            .http
            .post(format!("{}/chat/completions", self.base_url))
            .json(&body);
        if let Some(key) = &self.api_key {
            req = req.bearer_auth(key);
        }
        debug!("llm chat request");
        let resp = req.send().await.context("llm http")?;
        let status = resp.status();
        let text = resp.text().await?;
        if !status.is_success() {
            return Err(anyhow!("llm error {status}: {text}"));
        }
        serde_json::from_str(&text).context("parse llm response")
    }

    pub async fn embed(&self, input: &str) -> Result<Vec<f32>> {
        let body = json!({
            "model": self.embed_model,
            "input": input,
        });
        let mut req = self
            .http
            .post(format!("{}/embeddings", self.base_url))
            .json(&body);
        if let Some(key) = &self.api_key {
            req = req.bearer_auth(key);
        }
        let resp = req.send().await.context("embed http")?;
        let status = resp.status();
        let text = resp.text().await?;
        if !status.is_success() {
            return Err(anyhow!("embed error {status}: {text}"));
        }
        let parsed: EmbedResponse = serde_json::from_str(&text)?;
        parsed
            .data
            .into_iter()
            .next()
            .map(|d| d.embedding)
            .ok_or_else(|| anyhow!("empty embedding"))
    }
}

#[derive(Debug, Deserialize)]
pub struct ChatResponse {
    pub choices: Vec<Choice>,
}

#[derive(Debug, Deserialize)]
pub struct Choice {
    pub message: ChatMessage,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String,
    #[serde(default)]
    pub content: Option<String>,
    #[serde(default)]
    pub tool_calls: Option<Vec<ToolCall>>,
    #[serde(default)]
    pub reasoning_content: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ToolCall {
    pub id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub function: ToolFunction,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ToolFunction {
    pub name: String,
    pub arguments: String,
}

#[derive(Debug, Deserialize)]
struct EmbedResponse {
    data: Vec<EmbedData>,
}

#[derive(Debug, Deserialize)]
struct EmbedData {
    embedding: Vec<f32>,
}
