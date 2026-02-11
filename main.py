from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

app = FastAPI()

# Permet que el dashboard web pugui accedir a l'API
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],   # En producció es pot restringir
    allow_methods=["*"],
    allow_headers=["*"],
)

# Guardarem l'última mostra rebuda
latest = {}

@app.post("/upload")
async def upload(data: dict):
    global latest
    latest = data
    print("Rebut:", data)   # Es veurà als logs de Render
    return {"status": "ok"}

@app.get("/latest")
def get_latest():
    return latest

@app.get("/")
def root():
    return {"status": "API snowboard activa"}
