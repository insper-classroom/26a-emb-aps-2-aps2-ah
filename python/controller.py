import sys
import time
import serial
import pyautogui

PORT = "COM6"
BAUD = 9600

pyautogui.PAUSE = 0
pyautogui.FAILSAFE = True

BUTTON_MAP = {
    1: ("key", "a"),
    2: ("key", "x"),
    3: ("mouse", "left"),
    4: ("key", "i"),
}

pressed_keys = set()
pressed_mouse = set()


def handle_button_down(n):
    if n not in BUTTON_MAP:
        return
    kind, value = BUTTON_MAP[n]
    if kind == "key":
        pyautogui.keyDown(value)
        pressed_keys.add(value)
    elif kind == "mouse":
        pyautogui.mouseDown(button=value)
        pressed_mouse.add(value)


def handle_button_up(n):
    if n not in BUTTON_MAP:
        return
    kind, value = BUTTON_MAP[n]
    if kind == "key":
        pyautogui.keyUp(value)
        pressed_keys.discard(value)
    elif kind == "mouse":
        pyautogui.mouseUp(button=value)
        pressed_mouse.discard(value)


def handle_move(dx, dy):
    pyautogui.moveRel(dx, dy, duration=0)


def release_all():
    for value in list(pressed_keys):
        pyautogui.keyUp(value)
    for value in list(pressed_mouse):
        pyautogui.mouseUp(button=value)
    pressed_keys.clear()
    pressed_mouse.clear()


def parse_line(line):
    line = line.strip()
    if not line:
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
        else:
            print(f"[?] linha ignorada: {line}")
    except ValueError:
        print(f"[!] linha corrompida: {line}")


def main():
    print(f"Abrindo {PORT} @ {BAUD}...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"ERRO ao abrir a porta {PORT}: {e}")
        print("Confira se o controle esta pareado e se a COM esta correta.")
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