use anyhow::{anyhow, Context, Result};
use bar_config::MqttConfig;
use rumqttc::{AsyncClient, Event, Incoming, MqttOptions, QoS};
use serde_json::{json, Value};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::mpsc;
use tracing::{error, info, warn};
use url::Url;

#[derive(Debug, Clone)]
pub struct MqttEvent {
    pub topic: String,
    pub payload: Value,
}

pub struct MqttPlugin {
    client: AsyncClient,
    cfg: MqttConfig,
    rpc_id: AtomicU64,
}

impl MqttPlugin {
    pub async fn connect(cfg: MqttConfig) -> Result<(Self, mpsc::Receiver<MqttEvent>)> {
        let (host, port) = parse_broker(&cfg.broker)?;
        let mut opts = MqttOptions::new(&cfg.plugin_id, host, port);
        opts.set_keep_alive(Duration::from_secs(30));
        opts.set_clean_session(true);
        if !cfg.username.is_empty() {
            opts.set_credentials(&cfg.username, &cfg.password);
        }

        let (client, mut eventloop) = AsyncClient::new(opts, 64);
        let sub = expand_product(&cfg.events_subscribe, &cfg.product_id);
        client
            .subscribe(&sub, QoS::AtLeastOnce)
            .await
            .context("mqtt subscribe")?;
        info!(topic = %sub, "mqtt subscribed");

        let (tx, rx) = mpsc::channel(128);
        tokio::spawn(async move {
            loop {
                match eventloop.poll().await {
                    Ok(Event::Incoming(Incoming::Publish(p))) => {
                        let topic = p.topic.clone();
                        match serde_json::from_slice::<Value>(&p.payload) {
                            Ok(payload) => {
                                let _ = tx
                                    .send(MqttEvent {
                                        topic,
                                        payload,
                                    })
                                    .await;
                            }
                            Err(e) => warn!(%topic, error = %e, "bad mqtt json"),
                        }
                    }
                    Ok(_) => {}
                    Err(e) => {
                        error!(error = %e, "mqtt eventloop error");
                        tokio::time::sleep(Duration::from_secs(2)).await;
                    }
                }
            }
        });

        Ok((
            Self {
                client,
                cfg,
                rpc_id: AtomicU64::new(1),
            },
            rx,
        ))
    }

    pub async fn publish_command(
        &self,
        device_id: &str,
        method: &str,
        params: Value,
    ) -> Result<()> {
        let id = self.rpc_id.fetch_add(1, Ordering::Relaxed);
        let topic = format!(
            "{}/{}/rpc",
            expand_product(&self.cfg.commands_prefix, &self.cfg.product_id),
            device_id
        );
        let body = json!({
            "id": id,
            "method": method,
            "params": params,
        });
        self.client
            .publish(topic, QoS::AtLeastOnce, false, body.to_string())
            .await
            .context("publish rpc")?;
        Ok(())
    }

    pub async fn publish_attributes(&self, device_id: &str, attrs: Value) -> Result<()> {
        let topic = format!(
            "{}/{}/attributes/desired",
            expand_product(&self.cfg.commands_prefix, &self.cfg.product_id),
            device_id
        );
        self.client
            .publish(topic, QoS::AtLeastOnce, false, attrs.to_string())
            .await
            .context("publish attributes")?;
        Ok(())
    }

    pub fn product_id(&self) -> &str {
        &self.cfg.product_id
    }
}

pub type SharedMqtt = Arc<MqttPlugin>;

fn parse_broker(broker: &str) -> Result<(String, u16)> {
    let url = if broker.contains("://") {
        Url::parse(broker)?
    } else {
        Url::parse(&format!("mqtt://{broker}"))?
    };
    let host = url
        .host_str()
        .ok_or_else(|| anyhow!("broker host missing"))?
        .to_string();
    let port = url.port().unwrap_or(1883);
    Ok((host, port))
}

fn expand_product(template: &str, product_id: &str) -> String {
    template.replace("{product_id}", product_id)
}

/// Extract device_id from rules-bus event topics.
/// Supports:
/// - `platform/v1/events/{product}/{device}/event`
/// - `platform/v1/events/def/{product}/{device}/event`
pub fn device_id_from_topic(topic: &str) -> Option<String> {
    let parts: Vec<&str> = topic.split('/').collect();
    if parts.len() < 6 {
        return None;
    }
    // .../events/<maybe_account>/<product>/<device>/<kind>
    let events_idx = parts.iter().position(|p| *p == "events")?;
    let after = &parts[events_idx + 1..];
    if after.len() < 3 {
        return None;
    }
    // If first segment looks like account slug and next is product, device is index 2
    // Heuristic matching py-ctrl-center: device at parts[5] for def path.
    if after.len() >= 4 {
        // account/product/device/kind
        Some(after[2].to_string())
    } else {
        // product/device/kind
        Some(after[1].to_string())
    }
}
