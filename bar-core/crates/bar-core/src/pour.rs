use anyhow::Result;
use async_trait::async_trait;
use bar_dialog::PourExecutor;
use bar_mqtt::SharedMqtt;
use serde_json::json;

pub struct MqttPourExecutor {
    mqtt: SharedMqtt,
}

impl MqttPourExecutor {
    pub fn new(mqtt: SharedMqtt) -> Self {
        Self { mqtt }
    }
}

#[async_trait]
impl PourExecutor for MqttPourExecutor {
    async fn pour(&self, device_id: &str, tap_id: i64, ml: i64) -> Result<String> {
        self.mqtt
            .publish_command(
                device_id,
                "bar.pour",
                json!({
                    "tap_id": tap_id,
                    "ml": ml,
                }),
            )
            .await?;
        Ok("accepted (stub)".into())
    }
}
