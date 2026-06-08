# Controle Customizado — Papers, Please

**APS 2 + Expert (RTOS + IA) | Computação Embarcada — Insper**

---

## 1. O Jogo

Papers, Please é um jogo de simulação no qual o jogador atua como um inspetor de imigração da fictícia Arstotzka. A jogabilidade gira em torno de examinar documentos de viajantes, comparar informações, e decidir entre aprovar ou negar a entrada — carimbando o passaporte. Eventualmente o inspetor também precisa interrogar ou revistar suspeitos.

As ações principais do jogo são:

- Mover o cursor para inspecionar documentos.
- Clicar para arrastar documentos pela mesa.
- Carimbar APROVADO (tecla A).
- Carimbar NEGADO (tecla X).
- Interrogar/revistar (tecla I).

**Link do Jogo:** [Papers, Please na Steam](https://store.steampowered.com/app/239030/Papers_Please/)

---

## 2. Ideia do Controle

O controle é um dispositivo físico com fio que substitui o teclado e o mouse para jogar Papers, Please. A intenção é que o jogo seja jogado da forma mais imersiva possível: o jogador segura o controle como se fosse a prancheta do inspetor, inclina o controle para mover o cursor (via IMU) e pressiona botões físicos dedicados para os carimbos e ações.

Os dois botões principais — APPROVE (verde) e DENY (vermelho) — imitam os carimbos físicos do jogo, tornando a experiência intuitiva e temática. O botão CLICK permite arrastar e soltar documentos na mesa, e o botão INSPECT aciona interrogação ou revista.

O controle se comunica com o PC via **USB (serial)**, com um script Python no computador escutando as mensagens do controle e traduzindo-as em eventos reais de teclado e mouse no sistema operacional.

Além disso, um **módulo de IA embarcada** (rede neural treinada com Edge Impulse, rodando localmente na Pico) classifica gestos de movimento da IMU em tempo real. O modelo reconhece três classes de movimento e, ao detectar um gesto específico, dispara automaticamente uma ação no jogo e acende um LED indicando o gesto reconhecido — tudo sem conexão com a internet.

---

### Imagens


ideias: 

![alt text](image-1.png)

![alt text](image-2.png)

![alt text](image-5.png)
---


## 3. Entradas e Saídas

### Entradas (sensores e botões)

| Entrada       | Tipo              | Função no jogo                                          |
| ------------- | ----------------- | ------------------------------------------------------- |
| MPU6050 (IMU) | Analógico (I2C)   | Inclinar o controle move o cursor; alimenta a IA        |
| Botão APPROVE | Digital (IRQ)     | Carimba APROVADO (tecla A)                              |
| Botão DENY    | Digital (IRQ)     | Carimba NEGADO (tecla X)                                |
| Botão CLICK   | Digital (IRQ)     | Clique esquerdo (selecionar/arrastar documentos)        |
| Botão INSPECT | Digital (IRQ)     | Interrogar / revistar (tecla I)                         |
| Botão POWER   | Digital (polling) | Liga/desliga o controle                                 |

> Os botões de ação (APPROVE, DENY, CLICK, INSPECT) operam por **callback / interrupção**.
> O botão POWER é monitorado por **polling** dentro da `power_task` via `gpio_get()`.

### Saídas (atuadores)

| Saída              | Tipo    | Função                                                       |
| ------------------ | ------- | ------------------------------------------------------------ |
| LED de status      | Digital | Indica que o controle está ligado / ativo                    |
| LED de gesto (IA)  | Digital | Acende conforme o movimento classificado pela IA             |
| USB (serial)       | Serial  | Envia os eventos do controle para o PC (`controller.py`)     |

### Pinagem (hardware real)

| Componente        | Pino Pico (GP) |
| ----------------- | -------------- |
| Botão APPROVE     | GP13           |
| Botão DENY        | GP15           |
| Botão CLICK       | GP14           |
| Botão INSPECT     | GP12           |
| Botão POWER       | GP11           |
| LED de status     | GP17           |
| LED de gesto (IA) | GP16           |
| MPU6050 — SDA     | GP8 (I2C0)     |
| MPU6050 — SCL     | GP9 (I2C0)     |

---

## 4. Protocolo de Comunicação

A comunicação entre o controle e o PC é feita por **USB (serial)** usando um protocolo de texto ASCII orientado a linha, onde cada evento ocupa uma linha terminada por `\n`. O script `controller.py` lê essa porta serial e traduz os eventos em teclado/mouse.

### Formato geral

```
<TIPO>,<arg1>[,<arg2>]\n
```

### Mensagens definidas

| Mensagem    | Direção       | Significado                                                                   | Exemplo    |
| ----------- | ------------- | ----------------------------------------------------------------------------- | ---------- |
| `M,dx,dy\n` | Controle → PC | Movimento relativo do mouse (pixels, com sinal). `dx` e `dy` limitados a ±12. | `M,-3,5\n` |
| `BD,n\n`    | Controle → PC | Botão n foi pressionado (button down)                                         | `BD,1\n`   |
| `BU,n\n`    | Controle → PC | Botão n foi solto (button up)                                                 | `BU,1\n`   |
| `G,classe\n`| Controle → PC | Gesto reconhecido pela IA (ex.: `G,updown`)                                    | `G,updown\n` |
| `PWR,1\n`   | Controle → PC | Controle foi ligado                                                           | `PWR,1\n`  |
| `PWR,0\n`   | Controle → PC | Controle foi desligado                                                        | `PWR,0\n`  |

### Mapeamento de botões (n)

| n | Botão   | Ação no PC              |
| - | ------- | ----------------------- |
| 1 | APPROVE | Tecla A                 |
| 2 | DENY    | Tecla X                 |
| 3 | CLICK   | Botão esquerdo do mouse |
| 4 | INSPECT | Tecla I                 |

### Parâmetros do canal serial

| Parâmetro         | Valor     |
| ----------------- | --------- |
| Interface         | USB CDC   |
| Data bits         | 8         |
| Stop bits         | 1         |
| Paridade          | nenhuma   |
| Controle de fluxo | nenhum    |
| Terminador        | `\n` (LF) |

### Justificativa do protocolo

Optamos por texto ASCII (em vez de pacotes binários) por três motivos:

1. **Debug fácil** — basta abrir um monitor serial para ver os eventos chegando.
2. **Parser trivial no PC** — `line.split(",")` resolve.
3. **Robustez a ruído** — como cada evento termina em `\n`, um byte corrompido descarta no máximo uma linha, sem dessincronizar o stream.

O rate limiting e o clamp de movimento (±12 px) são aplicados no firmware antes de empacotar a mensagem, então o protocolo carrega apenas dados já saneados.

---

## 5. Inteligência Artificial Embarcada (Edge Impulse)

O expert de IA consiste em uma rede neural de reconhecimento de movimento rodando **localmente na Pico** (edge computing), sem qualquer conexão com a internet.

### Coleta e treino

- **Sensor:** acelerômetro do MPU6050 (3 eixos: accX, accY, accZ), amostrado a ~83 Hz.
- **Classes de movimento:**
  - `idle` — controle parado/em repouso
  - `updown` — movimento vertical (cima e baixo)
  - `wave` — movimento lateral (acenar)
- **Coleta de dados:** firmware de *data forwarding* enviando a aceleração pela serial; dados capturados via `edge-impulse-data-forwarder` (~10 amostras de 10 s por classe).
- **Pipeline (Impulse):** janela de 2000 ms → bloco de *Spectral Analysis* → bloco de *Classification* (rede neural densa).

### Resultados do modelo

| Métrica            | Valor   |
| ------------------ | ------- |
| Acurácia (validação) | 99.8% |
| Loss               | 0.06    |
| F1 score (médio)   | 1.00    |
| Latência (inferência) | ~17 ms |
| RAM                | ~2.7 KB |
| Flash              | ~15 KB  |

O modelo foi exportado como **biblioteca C++** (target Cortex-M33 / RP2350) e integrado ao firmware. A inferência roda dentro de uma task FreeRTOS dedicada, que consome as leituras da IMU, classifica o gesto e dispara a ação correspondente.

---

## 6. Diagrama de Blocos do Firmware

![diagrama](image-4.png)

### Tasks

| Task              | Prioridade | O que faz                                                                       |
| ----------------- | ---------- | ------------------------------------------------------------------------------- |
| `tx_task`         | 3          | Consome `xQueueTX` e escreve as mensagens na serial USB                         |
| `power_task`      | 2          | Trata botão POWER por polling; atualiza LED de status; envia `PWR,n\n`           |
| `imu_task`        | 1          | Lê MPU6050 a ~100 Hz, calibra no boot, aplica clamp e enfileira `M,dx,dy\n`      |
| `inference_task`  | 1          | Acumula o buffer da IMU, roda a inferência da IA, enfileira `G,classe\n` e acende o LED de gesto |
| `btn_task`        | 1          | Consome `xQueueButtons`, aplica debounce e enfileira `BD/BU,n\n`                 |

### ISR

| Callback       | Disparada por                           | O que faz                                                                                                   |
| -------------- | --------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `btn_callback` | IRQ de qualquer um dos 4 botões de ação | Monta evento com pino + estado + timestamp e dá `xQueueSendFromISR` em `xQueueButtons`. Mantida enxuta (< 10 instruções). |

### Filas e semáforos

| Recurso         | Tipo                             | Função                                                              |
| --------------- | -------------------------------- | ------------------------------------------------------------------- |
| `xQueueButtons` | Fila                             | Eventos crus de botão (ISR → `btn_task`)                            |
| `xQueueTX`      | Fila                             | Mensagens a serem transmitidas pela serial (qualquer task → `tx_task`) |
| `xQueuePower`   | Event group / notify             | Estado liga/desliga do controle, distribuído às tasks consumidoras   |
| `xSemRate`      | Semáforo de contagem             | Rate limiting do movimento da IMU                                   |

> Toda a comunicação entre tasks é feita exclusivamente por primitivas do FreeRTOS — **nenhuma variável global de estado**.

---

## 7. Software no PC (`controller.py`)

Script Python que:

- Abre a porta serial (USB) criada pela Pico.
- Lê as linhas do controle.
- Converte `M,dx,dy` em `pyautogui.moveRel()`.
- Converte `BD/BU,n` em `keyDown/keyUp` ou `mouseDown/mouseUp`.
- Interpreta `G,classe` (gesto da IA) e dispara a ação correspondente no jogo.
- Ao encerrar (Ctrl+C), libera qualquer tecla/botão que esteja pressionado.

Dependências: `pyserial`, `pyautogui`.

---

## 8. Experts escolhidos

**IA embarcada (Edge Impulse):** rede neural de reconhecimento de movimento rodando localmente na Pico, sem internet. Classifica três gestos da IMU (`idle`, `updown`, `wave`) com 99.8% de acurácia na validação. A inferência roda em uma task FreeRTOS dedicada e dispara ações no jogo / acende um LED conforme o gesto reconhecido.

**RTOS avançado:** múltiplas tasks com prioridades distintas, filas e semáforo de contagem. Comunicação inter-task exclusivamente por primitivas FreeRTOS — nenhuma variável global de estado. A IA é integrada como uma task de inferência dentro da arquitetura RTOS.
