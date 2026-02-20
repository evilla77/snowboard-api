import os
import requests
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
    print(f"\n--- NOVA DADA REBUDA (Gravant: {p.get('gravant')}) ---")
    print("JSON:", p)

    dispositiu_id = p.get("device_id")
    pair_code = p.get("pair_code")
    gravant = p.get("gravant", False) 
    
    if not dispositiu_id:
        raise HTTPException(status_code=400, detail="Falta 'device_id'")

    headers = {
        "apikey": KEY,
        "Authorization": f"Bearer {KEY}",
        "Content-Type": "application/json",
        "Prefer": "return=representation" # Crucial per rebre l'ID de la sessió creada
    }

    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    try:
        # 1. Busquem el dispositiu
        search_url = f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dispositiu_id}&select=id,usuari_id,status"
        r_search = requests.get(search_url, headers=headers)
        r_search.raise_for_status()
        data = r_search.json()

        # 2. Si el dispositiu és nou
        if not data:
            print(f"DEBUG: Dispositiu {dispositiu_id} no trobat, creant...")
            insert_data = {
                "dispositiu_id": dispositiu_id,
                "status": "pending",
                "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(),
                "last_seen_at": now.isoformat(),
                "is_recording": gravant
            }
            r_insert = requests.post(f"{URL}/rest/v1/dispositius", headers=headers, json=insert_data)
            print(f"DEBUG INSERT DISP: {r_insert.status_code} - {r_insert.text}")
            r_insert.raise_for_status()
            return {"ok": True, "status": "pending"}

        # 3. Si ja existeix, actualitzem dades de connexió i estat de gravació
        dev = data[0]
        update_data = {
            "last_seen_at": now.isoformat(),
            "is_recording": gravant
        }
        if pair_code:
            update_data["pair_code"] = pair_code
            update_data["pair_expires_at"] = expires.isoformat()

        update_url = f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dispositiu_id}"
        r_update = requests.patch(update_url, headers=headers, json=update_data)
        
        # --- MODIFICACIÓ 1: DIAGNÒSTIC DISPOSITIU ---
        print(f"DEBUG UPDATE DISP: {r_update.status_code} - {r_update.text}")
        r_update.raise_for_status()

        # Si no està vinculat, no fem res més
        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            print(f"DEBUG: Dispositiu no vinculat (Status: {dev.get('status')})")
            return {"ok": True, "status": "pending"}

        # --- LÒGICA DE SESSIONS I GPS (Només si està linked) ---
        
        # A. Busquem si hi ha sessió activa (sense ended_at)
        session_search_url = f"{URL}/rest/v1/sessions?dispositiu_id=eq.{dispositiu_id}&ended_at=is.null&select=id"
        r_sess_search = requests.get(session_search_url, headers=headers)
        active_sessions = r_sess_search.json()

        if gravant:
            # Si el botó està ON
            if not active_sessions:
                print("DEBUG: Creant nova sessió...")
                new_session_data = {
                    "dispositiu_id": dispositiu_id,
                    "usuari_id": dev.get("usuari_id"),
                    "started_at": now.isoformat()
                }
                r_new_sess = requests.post(f"{URL}/rest/v1/sessions", headers=headers, json=new_session_data)
                
                # --- MODIFICACIÓ 2: DIAGNÒSTIC SESSIÓ ---
                print(f"DEBUG NEW SESSION: {r_new_sess.status_code} - {r_new_sess.text}")
                
                res_sess = r_new_sess.json()
                if not res_sess:
                    print("ERROR: Supabase no ha retornat dades de la sessió!")
                    return {"ok": False, "error": "No s'ha pogut crear la sessió"}
                
                session_id = res_sess[0]["id"]
            else:
                session_id = active_sessions[0]["id"]
                print(f"DEBUG: Utilitzant sessió activa ID: {session_id}")

            # Guardem el punt GPS
            gps_data = {
                "session_id": session_id,
                "latitude": p.get("lat"),
                "longitude": p.get("lon"),
                "altitude": p.get("alt_m"),
                "speed": p.get("spd_kmh")
            }
            r_gps = requests.post(f"{URL}/rest/v1/punts_gps", headers=headers, json=gps_data)
            
            # --- MODIFICACIÓ 3: DIAGNÒSTIC PUNT GPS ---
            print(f"DEBUG INSERT GPS: {r_gps.status_code} - {r_gps.text}")
        
        else:
            # Si el botó està OFF, tanquem qualsevol sessió oberta
            if active_sessions:
                print(f"DEBUG: Tancant sessió {active_sessions[0]['id']}...")
                close_url = f"{URL}/rest/v1/sessions?id=eq.{active_sessions[0]['id']}"
                r_close = requests.patch(close_url, headers=headers, json={"ended_at": now.isoformat()})
                print(f"DEBUG CLOSE SESSION: {r_close.status_code}")

        return {"ok": True, "status": "linked", "gravant": gravant}

    except Exception as e:
        print(f"!!! ERROR EXCEPCIÓ: {str(e)}")
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/")
def root():
    return {"status": "ok", "info": "Servidor Snowboard Actiu"}