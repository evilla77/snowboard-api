import os
import requests
from datetime import datetime, timezone, timedelta
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

app = FastAPI()
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])

URL = os.environ.get("SUPABASE_URL")
KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY")
PAIR_MINUTES = 10

def calcular_direccio(graus):
    if graus < 0: return "--"
    dirs = ["N","NE","E","SE","S","SO","O","NO"]
    i = int((graus + 22.5) / 45.0)
    return dirs[i % 8]

@app.post("/upload")
async def upload(p: dict):
    # --- DISPLAY DE DADES REBUDES ---
    print(f"\n>>> JSON REBUT: {p}")
    
    dis_id = p.get("device_id")
    if not dis_id: raise HTTPException(status_code=400, detail="Falta device_id")
    
    gravant = p.get("gravant", False)
    pair_code = p.get("pair_code")
    headers = {"apikey": KEY, "Authorization": f"Bearer {KEY}", "Content-Type": "application/json", "Prefer": "return=representation"}
    
    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    try:
        r_dev = requests.get(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}&select=id,usuari_id,status", headers=headers)
        data = r_dev.json()

        if not data:
            print(f"Creant nou dispositiu: {dis_id}")
            requests.post(f"{URL}/rest/v1/dispositius", headers=headers, json={
                "dispositiu_id": dis_id, "status": "pending", "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(), "last_seen_at": now.isoformat(), "is_recording": gravant
            })
            return {"ok": True, "status": "pending"}

        dev = data[0]
        requests.patch(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}", headers=headers, json={
            "last_seen_at": now.isoformat(), "is_recording": gravant, "pair_code": pair_code, "pair_expires_at": expires.isoformat()
        })

        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            return {"ok": True, "status": "pending"}

        r_sess = requests.get(f"{URL}/rest/v1/sessions?dispositiu_id=eq.{dis_id}&ended_at=is.null&select=id", headers=headers)
        active_sessions = r_sess.json()

        if gravant:
            if not active_sessions:
                r_new = requests.post(f"{URL}/rest/v1/sessions", headers=headers, json={
                    "dispositiu_id": dis_id, "usuari_id": dev.get("usuari_id"), "started_at": now.isoformat()
                })
                session_id = r_new.json()[0]["id"]
                print(f"Sessió NOVA creada: {session_id}")
            else:
                session_id = active_sessions[0]["id"]

            direccio_text = calcular_direccio(p.get("course", -1))
            
            # --- DISPLAY DE TELEMETRIA ---
            print(f"Guardant punt: {p.get('temp')}°C | {p.get('pres')}hPa | Rumb: {direccio_text}")

            requests.post(f"{URL}/rest/v1/punts_gps", headers=headers, json={
                "session_id": session_id,
                "latitude": p.get("lat"),
                "longitude": p.get("lon"),
                "altitude": p.get("alt"),
                "speed": p.get("spd"),
                "temperature": p.get("temp"),
                "humidity": p.get("hum"),
                "pressure": p.get("pres"),
                "course_text": direccio_text
            })
        
        elif active_sessions:
            print(f"Tancant sessió: {active_sessions[0]['id']}")
            requests.patch(f"{URL}/rest/v1/sessions?id=eq.{active_sessions[0]['id']}", headers=headers, json={"ended_at": now.isoformat()})

        return {"ok": True, "status": "linked"}

    except Exception as e:
        print(f"ERROR: {e}")
        return {"ok": False, "error": str(e)}

@app.get("/")
def root(): return {"status": "ok"}