use anyhow::Result;
use bar_config::RagConfig;
use bar_llm::LlmClient;
use bar_storage::Database;
use std::path::Path;
use tracing::{info, warn};

pub struct RagIndex {
    cfg: RagConfig,
    db: Database,
    llm: LlmClient,
}

impl RagIndex {
    pub fn new(cfg: RagConfig, db: Database, llm: LlmClient) -> Self {
        Self { cfg, db, llm }
    }

    pub async fn reindex(&self) -> Result<usize> {
        self.db.clear_rag()?;
        let dir = Path::new(&self.cfg.knowledge_dir);
        if !dir.exists() {
            warn!(path = %dir.display(), "knowledge dir missing");
            return Ok(0);
        }
        let mut count = 0usize;
        let mut entries = tokio::fs::read_dir(dir).await?;
        while let Some(entry) = entries.next_entry().await? {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("md") {
                continue;
            }
            let text = tokio::fs::read_to_string(&path).await?;
            let source = path
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or("unknown")
                .to_string();
            for chunk in chunk_text(&text, self.cfg.chunk_size, self.cfg.chunk_overlap) {
                match self.llm.embed(&chunk).await {
                    Ok(emb) => {
                        self.db.insert_rag_chunk(&source, &chunk, &emb)?;
                        count += 1;
                    }
                    Err(e) => warn!(error = %e, "embed failed, skip chunk"),
                }
            }
        }
        info!(chunks = count, "rag reindexed");
        Ok(count)
    }

    pub async fn retrieve(&self, query: &str) -> Result<String> {
        let q = self.llm.embed(query).await?;
        let chunks = self.db.all_rag_chunks()?;
        if chunks.is_empty() {
            return Ok(String::new());
        }
        let mut scored: Vec<(f32, String)> = chunks
            .into_iter()
            .map(|c| (cosine(&q, &c.embedding), format!("[{}] {}", c.source, c.content)))
            .collect();
        scored.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(std::cmp::Ordering::Equal));
        let top: Vec<_> = scored
            .into_iter()
            .take(self.cfg.top_k)
            .filter(|(s, _)| *s > 0.1)
            .map(|(_, t)| t)
            .collect();
        Ok(top.join("\n---\n"))
    }
}

fn chunk_text(text: &str, size: usize, overlap: usize) -> Vec<String> {
    let chars: Vec<char> = text.chars().collect();
    if chars.is_empty() {
        return vec![];
    }
    let mut out = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        let end = (i + size).min(chars.len());
        out.push(chars[i..end].iter().collect());
        if end == chars.len() {
            break;
        }
        i = end.saturating_sub(overlap).max(i + 1);
    }
    out
}

fn cosine(a: &[f32], b: &[f32]) -> f32 {
    let n = a.len().min(b.len());
    if n == 0 {
        return 0.0;
    }
    let mut dot = 0.0;
    let mut na = 0.0;
    let mut nb = 0.0;
    for i in 0..n {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if na == 0.0 || nb == 0.0 {
        0.0
    } else {
        dot / (na.sqrt() * nb.sqrt())
    }
}
