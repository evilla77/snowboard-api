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
    dis_id = p.get("device_id")
    if not dis_id: raise HTTPException(status_code=400, detail="Falta device_id")
    
    gravant = p.get("gravant", False)
    pair_code = p.get("pair_code")
    headers = {
        "apikey": KEY, 
        "Authorization": f"Bearer {KEY}", 
        "Content-Type": "application/json", 
        "Prefer": "return=representation"
    }
    
    now = datetime.now(timezone.utc)
    expires = now + timedelta(minutes=PAIR_MINUTES)

    try:
        # 1. Estat del Dispositiu
        r_dev = requests.get(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}&select=id,usuari_id,status", headers=headers)
        data = r_dev.json()

        if not data:
            new_dev = {
                "dispositiu_id": dis_id, "status": "pending", "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(), "last_seen_at": now.isoformat(), "is_recording": gravant
            }
            requests.post(f"{URL}/rest/v1/dispositius", headers=headers, json=new_dev)
            return {"ok": True, "status": "pending"}

        dev = data[0]
        requests.patch(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}", headers=headers, json={
            "last_seen_at": now.isoformat(), "is_recording": gravant, "pair_code": pair_code, "pair_expires_at": expires.isoformat()
        })

        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            return {"ok": True, "status": "pending"}

        # 2. Gestió de Sessions
        r_sess = requests.get(f"{URL}/rest/v1/sessions?dispositiu_id=eq.{dis_id}&ended_at=is.null&select=id", headers=headers)
        active_sessions = r_sess.json()

        if gravant:
            if not active_sessions:
                r_new = requests.post(f"{URL}/rest/v1/sessions", headers=headers, json={
                    "dispositiu_id": dis_id, "usuari_id": dev.get("usuari_id"), "started_at": now.isoformat()
                })
                session_id = r_new.json()[0]["id"]
            else:
                session_id = active_sessions[0]["id"]

            # 3. Guardar punt GPS amb telemetria BME280
            gps_point = {
                "session_id": session_id,
                "latitude": p.get("lat"),
                "longitude": p.get("lon"),
                "altitude": p.get("alt"),
                "speed": p.get("spd"),
                "temperature": p.get("temp"),
                "humidity": p.get("hum"),
                "pressure": p.get("pres"),
                "course_text": calcular_direccio(p.get("course", -1)) # Lògica al servidor!
            }
            requests.post(f"{URL}/rest/v1/punts_gps", headers=headers, json=gps_point)
            print(f"Punt guardat: {p.get('temp')}C a {calcular_direccio(p.get('course', -1))}")

        elif active_sessions:
            requests.patch(f"{URL}/rest/v1/sessions?id=eq.{active_sessions[0]['id']}", headers=headers, json={"ended_at": now.isoformat()})

        return {"ok": True, "status": "linked"}

    except Exception as e:
        print(f"Error: {e}")
        return {"ok": False, "error": str(e)}

@app.get("/")
def root(): return {"status": "ok", "msg": "Arquitectura Offloading Activa"}