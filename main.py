import os
import requests  # Llibreria estàndard i super estable
from datetime import datetime, timezone, timedelta
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

latest = {}

# Configuració
URL = os.environ.get("SUPABASE_URL")
KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY")
PAIR_MINUTES = 10

@app.post("/upload")
async def upload(p: dict):
    global latest
    latest = p
    print("Rebut de l'ESP32:", p)

    dispositiu_id = p.get("device_id")
    pair_code = p.get("pair_code")
    
    if not dispositiu_id:
        raise HTTPException(status_code=400, detail="Falta 'device_id'")

    # Capçaleres per parlar amb Supabase directament
    headers = {
        "apikey": KEY,
        "Authorization": f"Bearer {KEY}",
        "Content-Type": "application/json",
        "Prefer": "return=representation"
    }

    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    try:
        # 1. Busquem el dispositiu (Crida REST directa)
        search_url = f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dispositiu_id}&select=id,usuari_id,status"
        r_search = requests.get(search_url, headers=headers)
        r_search.raise_for_status()
        data = r_search.json()

        # 2. Si el dispositiu és nou (Llista buida)
        if not data:
            print(f"Inserint nou dispositiu: {dispositiu_id}")
            insert_data = {
                "dispositiu_id": dispositiu_id,
                "status": "pending",
                "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(),
                "last_seen_at": now.isoformat(),
            }
            r_insert = requests.post(f"{URL}/rest/v1/dispositius", headers=headers, json=insert_data)
            r_insert.raise_for_status()
            return {"ok": True, "status": "pending"}

        # 3. Si ja existeix, actualitzem
        dev = data[0]
        update_data = {"last_seen_at": now.isoformat()}
        if pair_code:
            update_data["pair_code"] = pair_code
            update_data["pair_expires_at"] = expires.isoformat()

        update_url = f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dispositiu_id}"
        r_update = requests.patch(update_url, headers=headers, json=update_data)
        r_update.raise_for_status()

        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            return {"ok": True, "status": "pending"}

        return {"ok": True, "status": "linked"}

    except Exception as e:
        print(f"Error Directe Supabase: {e}")
        # Si hi ha error de resposta, mirem què diu el cos del missatge
        if 'r_search' in locals() and hasattr(r_search, 'text'): print(f"Detall: {r_search.text}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/")
def root():
    return {"status": "ok", "info": "Servidor Directe Actiu"}