import os
import httpx  # Importem httpx per configurar la connexió
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

# Aquesta variable guarda l'últim paquet rebut en memòria RAM (temporal)
latest = {}

SUPABASE_URL = os.environ.get("SUPABASE_URL")
SUPABASE_SERVICE_ROLE_KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY")

supabase: Client | None = None

if SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY:
    # CORRECCIÓ DEFINITIVA: Forcem HTTP/1.1 per evitar errors de protocol al Render
    # Creem un client HTTP personalitzat
    my_http_client = httpx.Client(http2=False, timeout=30.0)
    
    # Inicialitzem el client de Supabase passant-li el client HTTP
    # En versions recents de supabase-py, es fa així:
    supabase = create_client(
        SUPABASE_URL, 
        SUPABASE_SERVICE_ROLE_KEY,
        options={"http_client": my_http_client}
    )

PAIR_MINUTES = 10

def _require_supabase():
    if supabase is None:
        raise HTTPException(
            status_code=500,
            detail="Supabase no configurat. Revisa les variables d'entorn al Render.",
        )

@app.post("/upload")
async def upload(p: dict):
    global latest
    latest = p  
    print("Rebut de l'ESP32:", p)

    _require_supabase()

    dispositiu_id = p.get("device_id")
    pair_code = p.get("pair_code")
    lat = p.get("lat")
    lon = p.get("lon")

    if not dispositiu_id:
        raise HTTPException(status_code=400, detail="Falta 'device_id'")
    
    if lat is None or lon is None:
        raise HTTPException(status_code=400, detail="Falten coordenades")

    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    try:
        # 1. Busquem l'estat actual del dispositiu
        resp = (
            supabase.table("dispositius")
            .select("id, usuari_id, status")
            .eq("dispositiu_id", dispositiu_id)
            .limit(1)
            .execute()
        )

        # 2. Si el dispositiu és nou
        if not resp.data:
            if not pair_code:
                raise HTTPException(status_code=400, detail="Falta pair_code")

            supabase.table("dispositius").insert({
                "dispositiu_id": dispositiu_id,
                "status": "pending",
                "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(),
                "last_seen_at": now.isoformat(),
            }).execute()

            return {"ok": True, "status": "pending"}

        # 3. Si ja existeix, actualitzem
        dev = resp.data[0]
        update_obj = {"last_seen_at": now.isoformat()}
        
        if pair_code:
            update_obj["pair_code"] = pair_code
            update_obj["pair_expires_at"] = expires.isoformat()

        supabase.table("dispositius").update(update_obj)\
            .eq("dispositiu_id", dispositiu_id)\
            .execute()

        # 4. Verifiquem l'estat per respondre a l'ESP32
        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            return {"ok": True, "status": "pending"}

        return {"ok": True, "status": "linked"}

    except Exception as e:
        print(f"Error en la operació de Supabase: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/")
def root():
    return {"status": "ok", "info": "Servidor de seguiment actiu"}