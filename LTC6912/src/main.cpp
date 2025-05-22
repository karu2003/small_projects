#include <Arduino.h>

int ss = PB1; // Программный SS (Slave Select), также используется для LED
int sck = PB2; // Программный SCK
int mosi = PB0; // Программный MOSI

enum SPIMode {
  MODE0, // CPOL = 0, CPHA = 0
  MODE1, // CPOL = 0, CPHA = 1
  MODE2, // CPOL = 1, CPHA = 0
  MODE3  // CPOL = 1, CPHA = 1
};

void softwareSPITransfer(uint8_t data, SPIMode mode) {
  bool cpol = (mode == MODE2 || mode == MODE3); // Полярность тактового сигнала
  bool cpha = (mode == MODE1 || mode == MODE3); // Фаза тактового сигнала

  digitalWrite(ss, LOW); // Активируем устройство (и включаем LED)
  digitalWrite(sck, cpol); // Устанавливаем начальное состояние SCK

  for (int i = 7; i >= 0; i--) {
    if (!cpha) {
      digitalWrite(mosi, (data >> i) & 0x01); // Устанавливаем бит на MOSI
    }
    digitalWrite(sck, !cpol); // Подаем тактовый сигнал
    delayMicroseconds(1); // Небольшая задержка
    if (cpha) {
      digitalWrite(mosi, (data >> i) & 0x01); // Устанавливаем бит на MOSI
    }
    digitalWrite(sck, cpol); // Завершаем тактовый сигнал
    delayMicroseconds(1); // Небольшая задержка
  }

  digitalWrite(ss, HIGH); // Деактивируем устройство (и выключаем LED)
}

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  // pinMode(led, OUTPUT);
  pinMode(sck, OUTPUT); // Настройка SCK как выход
  pinMode(mosi, OUTPUT); // Настройка MOSI как выход
  pinMode(ss, OUTPUT); // Настройка SS как выход
  digitalWrite(ss, HIGH); // Устанавливаем SS в неактивное состояние (LED выключен)
}

// the loop routine runs over and over again forever:
void loop() {
    softwareSPITransfer(0x11, MODE3); // Отправка данных 0x11 через программный SPI в режиме MODE3
    delay(1000);
}
