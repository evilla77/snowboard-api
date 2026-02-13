import os
from datetime import datetime, timezone, timedelta

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from supabase import create_client, Client

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

latest = {}

# --- Supabase (service role) ---
SUPABASE_URL = os.environ.get("SUPABASE_URL")
SUPABASE_SERVICE_ROLE_KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY")

supabase: Client | None = None
if SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY:
    supabase = create_client(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY)

PAIR_MINUTES = 10  # caducitat del pair_code


def _require_supabase():
    if supabase is None:
        raise HTTPException(
            status_code=500,
            detail="Supabase no configurat. Falta SUPABASE_URL o SUPABASE_SERVICE_ROLE_KEY a Render.",
        )


@app.post("/upload")
async def upload(p: dict):
    """
    Rep un punt de l'ESP32.
    - Si el dispositiu no existeix: crea registre a 'dispositius' en mode pending amb pair_code i caducitat
    - Si existeix: actualitza last_seen_at i (si ve pair_code) també pair_code i caducitat
    - Si està 'linked': (més endavant) es guardarà el punt a punts_gps
    """
    global latest
    latest = p
    print("Rebut:", p)

    _require_supabase()

    device_id = p.get("device_id")
    pair_code = p.get("pair_code")
    lat = p.get("lat")
    lon = p.get("lon")

    if not device_id:
        raise HTTPException(status_code=400, detail="Falta 'device_id'")
    if lat is None or lon is None:
        raise HTTPException(status_code=400, detail="Falten 'lat' i/o 'lon'")

    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    # 1) El dispositiu existeix?
    resp = (
        supabase.table("dispositius")
        .select("id, user_id, status")
        .eq("device_id", device_id)
        .limit(1)
        .execute()
    )

    if not resp.data:
        # No existeix -> creem pending (necessitem pair_code)
        if not pair_code:
            raise HTTPException(status_code=400, detail="Falta 'pair_code' per crear dispositiu pending")

        supabase.table("dispositius").insert({
            "device_id": device_id,
            "status": "pending",
            "pair_code": pair_code,
            "pair_expires_at": expires.isoformat(),
            "last_seen_at": now.isoformat(),
        }).execute()

        return {"ok": True, "status": "pending", "msg": "Creat dispositiu pending"}

    dev = resp.data[0]

    # 2) Sempre actualitzem last_seen_at i, si tenim pair_code, també el refresquem
    update_obj = {"last_seen_at": now.isoformat()}
    if pair_code:
        update_obj["pair_code"] = pair_code
        update_obj["pair_expires_at"] = expires.isoformat()

    supabase.table("dispositius").update(update_obj).eq("device_id", device_id).execute()

    # 3) Si encara no està vinculat, no guardem punts (de moment)
    if dev.get("status") != "linked" or not dev.get("user_id"):
        return {"ok": True, "status": "pending", "msg": "Encara no vinculat"}

    # 4) Si està vinculat, aquí més endavant guardarem punts a punts_gps
    # (Ho activarem quan fem la pantalla de vincular)
    return {"ok": True, "status": "linked"}


@app.get("/latest")
def get_latest():
    return latest


@app.get("/")
def root():
    return {"status": "ok"}
