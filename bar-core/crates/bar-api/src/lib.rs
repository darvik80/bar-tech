use axum::extract::State;
use axum::routing::{get, post};
use axum::{Json, Router};
use bar_rag::RagIndex;
use bar_storage::{Database, Device};
use serde::Serialize;
use serde_json::{json, Value};
use std::sync::Arc;
use tower_http::trace::TraceLayer;

#[derive(Clone)]
pub struct ApiState {
    pub db: Database,
    pub rag: Arc<RagIndex>,
}

pub fn router(state: ApiState) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/v1/devices", get(list_devices))
        .route("/v1/rag/reload", post(reload_rag))
        .layer(TraceLayer::new_for_http())
        .with_state(state)
}

async fn health() -> Json<Value> {
    Json(json!({"ok": true, "service": "bar-core"}))
}

async fn list_devices(State(st): State<ApiState>) -> Json<Vec<Device>> {
    Json(st.db.list_devices().unwrap_or_default())
}

#[derive(Serialize)]
struct ReloadResp {
    chunks: usize,
}

async fn reload_rag(State(st): State<ApiState>) -> Json<ReloadResp> {
    let chunks = st.rag.reindex().await.unwrap_or(0);
    Json(ReloadResp { chunks })
}
