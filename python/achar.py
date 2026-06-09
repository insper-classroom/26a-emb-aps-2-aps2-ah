import pyautogui
import time

print("Passe o mouse sobre cada ponto. Ctrl+C para sair.")
print("Anote o X,Y de: carimbo APROVAR, carimbo NEGAR, e onde pega o passaporte.\n")

try:
    while True:
        x, y = pyautogui.position()
        print(f"X={x}  Y={y}   ", end="\r")
        time.sleep(0.1)
except KeyboardInterrupt:
    print("\nPronto!")