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
    if graus is None or graus < 0: return "--"
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
        # 1. Busquem l'estat del dispositiu a la taula 'dispositius'
        r_dev = requests.get(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}&select=id,usuari_id,status", headers=headers)
        data = r_dev.json()

        if not data:
            print(f"Creant nou dispositiu pendent de vinculació: {dis_id}")
            requests.post(f"{URL}/rest/v1/dispositius", headers=headers, json={
                "dispositiu_id": dis_id, 
                "status": "pending", 
                "pair_code": pair_code,
                "pair_expires_at": expires.isoformat(), 
                "last_seen_at": now.isoformat(), 
                "is_recording": gravant
            })
            return {"ok": True, "status": "pending"}

        dev = data[0]
        
        # Actualitzem l'estat de presència del maquinari a la taula mestra
        update_data = {
            "last_seen_at": now.isoformat(), 
            "is_recording": gravant
        }
        if dev.get("status") != "linked":
            update_data["pair_code"] = pair_code
            update_data["pair_expires_at"] = expires.isoformat()

        requests.patch(f"{URL}/rest/v1/dispositius?dispositiu_id=eq.{dis_id}", headers=headers, json=update_data)

        # 🚨 CONTROL DE SEGURETAT: Si no està vinculat o falta l'usuari_id, no enviem res a live_ping
        if dev.get("status") != "linked" or not dev.get("usuari_id"):
            print(f"El dispositiu {dis_id} està 'pending' o no té usuari assignat. Es cancel·la el flux live.")
            return {"ok": True, "status": "pending"}

        # Busquem si hi ha alguna sessió oberta (sense data de finalització)
        r_sess = requests.get(f"{URL}/rest/v1/sessions?dispositiu_id=eq.{dis_id}&ended_at=is.null&select=id", headers=headers)
        active_sessions = r_sess.json()

        # ==========================================
        # CAS A: EL DISPOSITIU ESTÀ GRAVANT RUTA
        # ==========================================
        if gravant:
            if not active_sessions:
                r_new = requests.post(f"{URL}/rest/v1/sessions", headers=headers, json={
                    "dispositiu_id": dis_id, "usuari_id": dev.get("usuari_id"), "started_at": now.isoformat()
                })
                
                res_json = r_new.json()
                if isinstance(res_json, list) and len(res_json) > 0:
                    session_id = res_json[0]["id"]
                else:
                    r_retry = requests.get(f"{URL}/rest/v1/sessions?dispositiu_id=eq.{dis_id}&ended_at=is.null&select=id", headers=headers)
                    session_id = r_retry.json()[0]["id"]
                
                print(f"Sessió NOVA creada i vinculada: {session_id}")
            else:
                session_id = active_sessions[0]["id"]

            # 1. Desar dades agregades a la sessió activa
            session_update = {
                "jump_count": p.get("jump_count", 0),
                "straight_airs": p.get("straight_airs", 0),
                "jumps_180": p.get("jumps_180", 0),
                "jumps_360": p.get("jumps_360", 0),
                "jumps_540": p.get("jumps_540", 0),
                "jumps_720": p.get("jumps_720", 0),
                "max_airtime": p.get("max_airtime", 0.0),
                "max_spin": p.get("max_spin", 0.0),
                "max_landing_g": p.get("max_landing_g", 0.0)
            }
            requests.patch(f"{URL}/rest/v1/sessions?id=eq.{session_id}", headers=headers, json=session_update)

            # 2. Guardar punt històric a punts_gps
            direccio_text = calcular_direccio(p.get("course", -1))
            requests.post(f"{URL}/rest/v1/punts_gps", headers=headers, json={
                "session_id": session_id,
                "usuari_id": dev.get("usuari_id"),
                "timestamp": now.isoformat(),
                "latitude": p.get("lat"),
                "longitude": p.get("lon"),
                "altitude": p.get("alt"),
                "speed": p.get("spd"),
                "temperature": p.get("temp"),
                "humidity": p.get("hum"),
                "pressure": p.get("pres"),
                "course_text": direccio_text
            })
        
        # ==========================================
        # CAS B: EL DISPOSITIU ESTÀ EN STANDBY (LIVE PING)
        # ==========================================
        else:
            print(f"Standby actiu per a {dis_id}: Actualitzant live_ping per a l'usuari: {dev.get('usuari_id')}...")
            
            direccio_text = calcular_direccio(p.get("course", -1))
            
            # Cambiem Prefer a 'resolution=merge-duplicates' per utilitzar la teva clau primària correctament
            headers_upsert = {
                "apikey": KEY,
                "Authorization": f"Bearer {KEY}",
                "Content-Type": "application/json",
                "Prefer": "action=upsert,resolution=merge-duplicates"
            }
            
            # Sincronitzat al 100% amb el canvi a 'dispositiu_id' de Supabase
            dades_ping = {
                "dispositiu_id": dis_id,            # ◄── Sincronitzat amb la teva columna de clau primària
                "usuari_id": dev.get("usuari_id"),  # ◄── S'envia la ID de l'usuari propietari (Adéu al NULL!)
                "updated_at": now.isoformat(),
                "lat": p.get("lat"),
                "lon": p.get("lon"),
                "alt": p.get("alt"),
                "spd": p.get("spd"),
                "temp": p.get("temp"),
                "hum": p.get("hum"),
                "pres": p.get("pres"),
                "course_text": direccio_text
            }
            
            r_ping = requests.post(
                f"{URL}/rest/v1/live_ping", 
                headers=headers_upsert, 
                json=dades_ping
            )
            print(f"-> Resposta de live_ping a Supabase: Status {r_ping.status_code}")
            if r_ping.status_code >= 400:
                print(f"❌ Error detallat de Supabase a l'escriptura live_ping: {r_ping.text}")

            # Si entra en standby i venia d'una gravació, tanquem la sessió de forma segura
            if active_sessions:
                print(f"Tancant sessió activa per entrada en standby: {active_sessions[0]['id']}")
                requests.patch(f"{URL}/rest/v1/sessions?id=eq.{active_sessions[0]['id']}", headers=headers, json={"ended_at": now.isoformat()})

        return {"ok": True, "status": "linked"}

    except Exception as e:
        print(f"💥 ERROR CRÍTIC AL MAIN: {e}")
        return {"ok": False, "error": str(e)}

@app.get("/")
def root(): 
    return {"status": "ok", "service": "telemetry-receiver"}