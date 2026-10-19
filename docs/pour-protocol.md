# Pour protocol (phase 1 — stub)

Hardware GPIO / valves are **out of scope**. Firmware and bar-core implement the contract so dialog can call pour safely.

## RPC: `bar.pour`

**Topic:** `.../commands/{product_id}/{device_id}/rpc`

**Request:**

```json
{
  "id": 10,
  "method": "bar.pour",
  "params": {
    "tap_id": 1,
    "ml": 300
  }
}
```

Alternate: `duration_ms` instead of `ml` (firmware accepts either; stub ignores both).

**Response (firmware stub):**

```json
{
  "id": 10,
  "result": {
    "ok": true,
    "status": "stub",
    "tap_id": 1,
    "ml": 300
  }
}
```

On unknown tap or bad params:

```json
{
  "id": 10,
  "error": {
    "code": -1,
    "message": "invalid params"
  }
}
```

## Optional event

Device may publish:

```json
{
  "type": "pour_stub",
  "tap_id": 1,
  "ml": 300,
  "ts": 0
}
```

## Dialog tool (bar-core)

LLM tool name: `pour`

| Arg | Type | Notes |
|-----|------|-------|
| `tap_id` | integer | Tap index (1-based) |
| `ml` | integer | Volume hint for future hardware |

Execution publishes `bar.pour` RPC and returns `"accepted (stub)"` to the model.

## Phase 2 (later)

- Map `tap_id` → GPIO / valve driver
- Flow meter feedback, cancel, timeouts
- Safety interlocks (glass present, max ml/day)
