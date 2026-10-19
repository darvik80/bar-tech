use anyhow::{Context, Result};
use bar_config::DbConfig;
use chrono::Utc;
use rusqlite::{params, Connection};
use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::{Arc, Mutex};
use tracing::info;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Device {
    pub device_id: String,
    pub locale: String,
    pub voice_id: String,
}

#[derive(Debug, Clone)]
pub struct Message {
    pub device_id: String,
    pub role: String,
    pub content: String,
}

#[derive(Debug, Clone)]
pub struct RagChunkRow {
    pub id: i64,
    pub source: String,
    pub content: String,
    pub embedding: Vec<f32>,
}

#[derive(Clone)]
pub struct Database {
    conn: Arc<Mutex<Connection>>,
    cfg: DbConfig,
}

impl Database {
    pub fn open(cfg: DbConfig) -> Result<Self> {
        if let Some(parent) = Path::new(&cfg.path).parent() {
            std::fs::create_dir_all(parent).ok();
        }
        let conn = Connection::open(&cfg.path).context("open sqlite")?;
        conn.execute_batch(
            r#"
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS device (
                device_id TEXT PRIMARY KEY,
                locale TEXT NOT NULL DEFAULT 'ru',
                voice_id TEXT NOT NULL DEFAULT 'default',
                last_active_at TEXT
            );
            CREATE TABLE IF NOT EXISTS message (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_message_device ON message(device_id, created_at);
            CREATE TABLE IF NOT EXISTS rag_chunk (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                source TEXT NOT NULL,
                content TEXT NOT NULL,
                embedding BLOB NOT NULL
            );
            "#,
        )?;
        info!(path = %cfg.path, "sqlite ready");
        Ok(Self {
            conn: Arc::new(Mutex::new(conn)),
            cfg,
        })
    }

    pub fn get_or_create_device(&self, device_id: &str) -> Result<Device> {
        let conn = self.conn.lock().unwrap();
        let existing = conn.query_row(
            "SELECT device_id, locale, voice_id FROM device WHERE device_id=?1",
            params![device_id],
            |r| {
                Ok(Device {
                    device_id: r.get(0)?,
                    locale: r.get(1)?,
                    voice_id: r.get(2)?,
                })
            },
        );
        if let Ok(d) = existing {
            return Ok(d);
        }
        conn.execute(
            "INSERT INTO device(device_id, locale, voice_id, last_active_at) VALUES(?1,'ru','default',?2)",
            params![device_id, Utc::now().to_rfc3339()],
        )?;
        Ok(Device {
            device_id: device_id.to_string(),
            locale: "ru".into(),
            voice_id: "default".into(),
        })
    }

    pub fn list_devices(&self) -> Result<Vec<Device>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT device_id, locale, voice_id FROM device")?;
        let rows = stmt.query_map([], |r| {
            Ok(Device {
                device_id: r.get(0)?,
                locale: r.get(1)?,
                voice_id: r.get(2)?,
            })
        })?;
        Ok(rows.filter_map(|r| r.ok()).collect())
    }

    pub fn add_message(&self, msg: &Message) -> Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO message(device_id, role, content, created_at) VALUES(?1,?2,?3,?4)",
            params![msg.device_id, msg.role, msg.content, Utc::now().to_rfc3339()],
        )?;
        Ok(())
    }

    pub fn get_messages(&self, device_id: &str, limit: usize) -> Result<Vec<Message>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare(
            "SELECT device_id, role, content FROM message WHERE device_id=?1 ORDER BY id DESC LIMIT ?2",
        )?;
        let rows = stmt.query_map(params![device_id, limit as i64], |r| {
            Ok(Message {
                device_id: r.get(0)?,
                role: r.get(1)?,
                content: r.get(2)?,
            })
        })?;
        let mut msgs: Vec<_> = rows.filter_map(|r| r.ok()).collect();
        msgs.reverse();
        Ok(msgs)
    }

    pub fn clear_rag(&self) -> Result<()> {
        let conn = self.conn.lock().unwrap();
        conn.execute("DELETE FROM rag_chunk", [])?;
        Ok(())
    }

    pub fn insert_rag_chunk(&self, source: &str, content: &str, embedding: &[f32]) -> Result<()> {
        let blob = f32_slice_to_bytes(embedding);
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO rag_chunk(source, content, embedding) VALUES(?1,?2,?3)",
            params![source, content, blob],
        )?;
        Ok(())
    }

    pub fn all_rag_chunks(&self) -> Result<Vec<RagChunkRow>> {
        let conn = self.conn.lock().unwrap();
        let mut stmt = conn.prepare("SELECT id, source, content, embedding FROM rag_chunk")?;
        let rows = stmt.query_map([], |r| {
            let blob: Vec<u8> = r.get(3)?;
            Ok(RagChunkRow {
                id: r.get(0)?,
                source: r.get(1)?,
                content: r.get(2)?,
                embedding: bytes_to_f32_vec(&blob),
            })
        })?;
        Ok(rows.filter_map(|r| r.ok()).collect())
    }

    pub fn purge_old(&self) -> Result<()> {
        let conn = self.conn.lock().unwrap();
        let cutoff = Utc::now() - chrono::Duration::days(self.cfg.retention_days as i64);
        conn.execute(
            "DELETE FROM message WHERE created_at < ?1",
            params![cutoff.to_rfc3339()],
        )?;
        Ok(())
    }

    pub fn max_turns(&self) -> usize {
        self.cfg.max_turns
    }
}

fn f32_slice_to_bytes(v: &[f32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(v.len() * 4);
    for f in v {
        out.extend_from_slice(&f.to_le_bytes());
    }
    out
}

fn bytes_to_f32_vec(b: &[u8]) -> Vec<f32> {
    b.chunks_exact(4)
        .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect()
}
