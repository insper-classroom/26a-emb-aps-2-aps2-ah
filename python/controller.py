import sys
import time
import serial
import pyautogui

PORT = "COM4"        # troque pela COM real da sua Pico
BAUD = 115200

pyautogui.PAUSE = 0
pyautogui.FAILSAFE = True

# Posicoes fixas na tela (medidas no jogo)
POS_APROVAR = (1568, 415)   # botao verde (APPROVE)
POS_NEGAR   = (1208, 415)   # botao vermelho (DENY)
POS_ABRIR   = (1800, 528)   # abrir a bandeja de carimbos

DEADZONE = 2   # ignora movimento pequeno (drift)

pressed_mouse = set()

# Quando > 0, o movimento do giroscopio fica bloqueado (durante um carimbo)
bloqueio_mov = 0.0


def clicar_em(pos):
    """Move pro ponto e clica, dando tempo do jogo registrar."""
    global bloqueio_mov
    bloqueio_mov = time.time() + 0.4   # trava o movimento por 0.4s
    try:
        pyautogui.moveTo(pos[0], pos[1])
        time.sleep(0.08)
        pyautogui.click()
        time.sleep(0.05)
    except pyautogui.FailSafeException:
        pass


def handle_button_down(n):
    if n == 1:        # APPROVE (verde) -> carimba aprovar
        clicar_em(POS_APROVAR)
    elif n == 2:      # DENY (vermelho) -> carimba negar
        clicar_em(POS_NEGAR)
    elif n == 3:      # CLICK -> segura clique onde o cursor esta (arrastar)
        try:
            pyautogui.mouseDown(button="left")
            pressed_mouse.add("left")
        except pyautogui.FailSafeException:
            pass
    elif n == 4:      # INSPECT (GP12) -> abre a bandeja de carimbos
        clicar_em(POS_ABRIR)


def handle_button_up(n):
    if n == 3:
        if "left" in pressed_mouse:
            try:
                pyautogui.mouseUp(button="left")
            except pyautogui.FailSafeException:
                pass
            pressed_mouse.discard("left")


def handle_move(dx, dy):
    # Ignora movimento enquanto um carimbo esta acontecendo
    if time.time() < bloqueio_mov:
        return
    if abs(dx) < DEADZONE:
        dx = 0
    if abs(dy) < DEADZONE:
        dy = 0
    if dx == 0 and dy == 0:
        return
    try:
        pyautogui.moveRel(dx, dy, duration=0)
    except pyautogui.FailSafeException:
        pass


def release_all():
    for value in list(pressed_mouse):
        try:
            pyautogui.mouseUp(button=value)
        except pyautogui.FailSafeException:
            pass
    pressed_mouse.clear()


def parse_line(line):
    line = line.strip()
    if not line:
        return
    if line.startswith("[") or line.startswith("="):
        return

    parts = line.split(",")
    tipo = parts[0]

    try:
        if tipo == "M" and len(parts) == 3:
            handle_move(int(parts[1]), int(parts[2]))
        elif tipo == "BD" and len(parts) == 2:
            handle_button_down(int(parts[1]))
        elif tipo == "BU" and len(parts) == 2:
            handle_button_up(int(parts[1]))
        elif tipo == "PWR" and len(parts) == 2:
            estado = int(parts[1])
            if estado == 0:
                release_all()
            print(f"[PWR] controle {'ligado' if estado == 1 else 'desligado'}")
    except ValueError:
        pass


def main():
    print(f"Abrindo {PORT} @ {BAUD}...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"ERRO ao abrir a porta {PORT}: {e}")
        print("Confira a COM da Pico e feche o Serial Monitor antes de rodar.")
        sys.exit(1)

    print("Conectado! Controle ativo. Ctrl+C para sair.")

    buffer = ""
    try:
        while True:
            data = ser.read(256)
            if data:
                buffer += data.decode("utf-8", errors="replace")
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    parse_line(line)
            else:
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\nEncerrando...")
    finally:
        release_all()
        ser.close()
        print("Porta fechada. Tudo solto.")


if __name__ == "__main__":
    main()