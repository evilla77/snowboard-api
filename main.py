import os
from datetime import datetime, timezone

from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from supabase import create_client, Client

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Guarda l'últim punt rebut (per debug / /latest)
latest = {}

# --- Supabase (service role) ---
SUPABASE_URL = os.environ.get("SUPABASE_URL")
SUPABASE_SERVICE_ROLE_KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY")

# (Opcional però recomanat) secret per evitar spam d'uploads
INGEST_SECRET = os.environ.get("INGEST_SECRET")  # ex: "paraula_secreta"

supabase: Client | None = None

if SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY:
    supabase = create_client(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY)


def _require_supabase():
    if supabase is None:
        raise HTTPException(
            status_code=500,
            detail="Supabase no configurat. Falta SUPABASE_URL o SUPABASE_SERVICE_ROLE_KEY a Render.",
        )


@app.post("/upload")
async def upload(p: dict, x_ingest_secret: str | None = Header(default=None)):
    """
    Rep un punt GPS de l'ESP32:
      - valida device_id
      - mira a Supabase (taula 'dispositius') quin user_id correspon
      - desa el punt a 'punts_gps'
    """
    global latest
    latest = p
    print("Rebut:", p)

    # --- Protecció opcional ---
    # Si has definit INGEST_SECRET a Render, obliguem a enviar header x-ingest-secret
    if INGEST_SECRET:
        if x_ingest_secret != INGEST_SECRET:
            raise HTTPException(status_code=401, detail="No autoritzat (secret incorrecte)")

    _require_supabase()

    device_id = p.get("device_id")
    lat = p.get("lat")
    lon = p.get("lon")

    if not device_id:
        raise HTTPException(status_code=400, detail="Falta 'dispositiu_id'")
    if lat is None or lon is None:
        raise HTTPException(status_code=400, detail="Falten 'lat' i/o 'lon'")

    # 1) Busquem el dispositiu a 'dispositius'
    try:
        resp_dev = (
            supabase.table("dispositius")
            .select("user_id")
            .eq("device_id", device_id)
            .limit(1)
            .execute()
        )
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error consultat Supabase (dispositius): {e}")

    if not resp_dev.data:
        raise HTTPException(status_code=404, detail="Dispositiu no vinculat (no existeix a 'dispositius')")

    user_id = resp_dev.data[0]["user_id"]

    # 2) Inserim el punt a 'punts_gps'
    fila = {
        "user_id": user_id,
        "device_id": device_id,
        "t_ms": p.get("t_ms"),
        "lat": lat,
        "lon": lon,
        "alt_m": p.get("alt_m"),
        "spd_kmh": p.get("spd_kmh"),
        "course_deg": p.get("course_deg"),
        "hour": p.get("hour"),
        "min": p.get("min"),
        "sec": p.get("sec"),
        "created_at": datetime.now(timezone.utc).isoformat(),
    }

    try:
        resp_ins = supabase.table("punts_gps").insert(fila).execute()
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error inserint a Supabase (punts_gps): {e}")

    # Si vols, pots retornar també el user_id per debug
    return {"ok": True, "user_id": user_id}


@app.get("/latest")
def get_latest():
    return latest


@app.get("/")
def root():
    return {"status": "ok"}
