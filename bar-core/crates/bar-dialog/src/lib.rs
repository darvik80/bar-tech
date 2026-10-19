use anyhow::Result;
use async_trait::async_trait;
use bar_llm::LlmClient;
use bar_rag::RagIndex;
use bar_storage::{Database, Message};
use serde_json::{json, Value};
use std::sync::Arc;
use tracing::{info, warn};

const MAX_TOOL_ITERATIONS: usize = 5;

const SYSTEM_PROMPT_RU: &str = r#"Вы — опытный бармен из рустикального, старомодного паба. Вы — душа этого места, но душа негромкая.

### КЛЮЧЕВЫЕ ЛИЧНОСТНЫЕ ЧЕРТЫ:
1. Дружелюбие без фамильярности.
2. Лаконичность: ответы короткие, 1–2 фразы.
3. Идеальный слушатель.
4. Привязанность к месту за барной стойкой.

### ПРАВИЛА:
- Только простой текст, без markdown.
- Используйте инструмент pour, когда гость просит налить/разлить напиток.
- Никогда не выходите из роли.
"#;

#[async_trait]
pub trait PourExecutor: Send + Sync {
    async fn pour(&self, device_id: &str, tap_id: i64, ml: i64) -> Result<String>;
}

pub struct DialogEngine {
    db: Database,
    llm: LlmClient,
    rag: Arc<RagIndex>,
    pour: Arc<dyn PourExecutor>,
}

impl DialogEngine {
    pub fn new(
        db: Database,
        llm: LlmClient,
        rag: Arc<RagIndex>,
        pour: Arc<dyn PourExecutor>,
    ) -> Self {
        Self {
            db,
            llm,
            rag,
            pour,
        }
    }

    pub async fn process_utterance(&self, device_id: &str, text: &str) -> Result<String> {
        let _device = self.db.get_or_create_device(device_id)?;
        self.db.add_message(&Message {
            device_id: device_id.to_string(),
            role: "user".into(),
            content: text.to_string(),
        })?;

        let context = self.rag.retrieve(text).await.unwrap_or_default();
        let mut system = SYSTEM_PROMPT_RU.to_string();
        if !context.is_empty() {
            system.push_str("\n\n### КОНТЕКСТ ИЗ БАЗЫ ЗНАНИЙ:\n");
            system.push_str(&context);
        }

        let history = self.db.get_messages(device_id, self.db.max_turns())?;
        let mut messages: Vec<Value> = vec![json!({"role":"system","content": system})];
        for m in history {
            messages.push(json!({"role": m.role, "content": m.content}));
        }

        let tools = pour_tool_schema();
        let mut reply_text = String::new();

        for _ in 0..MAX_TOOL_ITERATIONS {
            let resp = self.llm.chat_completion(&messages, Some(&tools)).await?;
            let message = resp
                .choices
                .first()
                .map(|c| c.message.clone())
                .ok_or_else(|| anyhow::anyhow!("empty llm choices"))?;

            if let Some(tool_calls) = &message.tool_calls {
                if !tool_calls.is_empty() {
                    messages.push(serde_json::to_value(&message)?);
                    for tc in tool_calls {
                        let result = self
                            .execute_tool(device_id, &tc.function.name, &tc.function.arguments)
                            .await;
                        let content = match result {
                            Ok(s) => s,
                            Err(e) => format!("error: {e}"),
                        };
                        messages.push(json!({
                            "role": "tool",
                            "tool_call_id": tc.id,
                            "content": content,
                        }));
                    }
                    continue;
                }
            }

            reply_text = message
                .content
                .clone()
                .or(message.reasoning_content.clone())
                .unwrap_or_default();
            break;
        }

        if reply_text.is_empty() {
            reply_text = "Не расслышал, повтори пожалуйста".into();
        }

        self.db.add_message(&Message {
            device_id: device_id.to_string(),
            role: "assistant".into(),
            content: reply_text.clone(),
        })?;
        info!(%device_id, reply = %reply_text.chars().take(80).collect::<String>(), "dialog done");
        Ok(reply_text)
    }

    async fn execute_tool(&self, device_id: &str, name: &str, arguments: &str) -> Result<String> {
        match name {
            "pour" => {
                let args: Value = serde_json::from_str(arguments).unwrap_or(json!({}));
                let tap_id = args.get("tap_id").and_then(|v| v.as_i64()).unwrap_or(1);
                let ml = args.get("ml").and_then(|v| v.as_i64()).unwrap_or(300);
                info!(%device_id, tap_id, ml, "tool pour");
                self.pour.pour(device_id, tap_id, ml).await
            }
            other => {
                warn!(tool = other, "unknown tool");
                Ok(format!("unknown tool {other}"))
            }
        }
    }
}

fn pour_tool_schema() -> Vec<Value> {
    vec![json!({
        "type": "function",
        "function": {
            "name": "pour",
            "description": "Налить напиток из крана (stub на устройстве).",
            "parameters": {
                "type": "object",
                "properties": {
                    "tap_id": { "type": "integer", "description": "Номер крана, с 1" },
                    "ml": { "type": "integer", "description": "Объём в мл" }
                },
                "required": ["tap_id", "ml"]
            }
        }
    })]
}
