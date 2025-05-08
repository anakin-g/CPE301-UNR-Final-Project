#include <LiquidCrystal.h>
#include <Stepper.h>
#include <RTClib.h>
#include <string.h>
#include <DHT.h>

//Pin/Constants Mappings (tried to make it as clear as possible, change as needed)
#define LCD_LEN 16
#define LCD_RS 12
#define LCD_EN 11
#define LCD_D4 6
#define LCD_D5 5
#define LCD_D6 4
#define LCD_D7 3

#define DHT_PIN 7
#define DHTTYPE DHT11

#define FAN_MASK 0x10

#define LED_GREEN 0x80
#define LED_YELLOW 0x20
#define LED_RED 0x08
#define LED_BLUE 0x02

#define BUTTON_START 0x08
#define BUTTON_RESET 0x04
#define BUTTON_CTRL 0x02
#define PCI_MASK 0x0C

#define RBE 0x80
#define TBE 0x20

volatile unsigned char *PORT_B = (unsigned char *) 0x25;
volatile unsigned char *DDR_B  = (unsigned char *) 0x24;
volatile unsigned char *PIN_B  = (unsigned char *) 0x23;

volatile unsigned char *PORT_C = (unsigned char *) 0x28;
volatile unsigned char *DDR_C  = (unsigned char *) 0x27;

volatile unsigned char* my_ADMUX    = (unsigned char*) 0x7C;
volatile unsigned char* my_ADCSRB   = (unsigned char*) 0x7B;
volatile unsigned char* my_ADCSRA   = (unsigned char*) 0x7A;
volatile unsigned int*  my_ADC_DATA  = (unsigned int*)  0x78;

volatile unsigned char *myUCSR0A = (unsigned char *) 0xC0;
volatile unsigned char *myUCSR0B = (unsigned char *) 0xC1;
volatile unsigned char *myUSCR0C = (unsigned char *) 0xC2;
volatile unsigned char *myUBRR_0  = (unsigned char *) 0xC4;
volatile unsigned char *myUDR_0   = (unsigned char *) 0xC6;

volatile unsigned char *myPCICR   = (unsigned char *) 0x68;
volatile unsigned char *myPCMSK0  = (unsigned char *) 0x6B;

enum DeviceState {SYS_DISABLED, SYS_IDLE, SYS_ERROR, SYS_RUNNING};

// LCD, sensors, stepper
LiquidCrystal display(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
Stepper ventStepper(2038, 28, 26, 24, 22);
DHT climateSensor(DHT_PIN, DHTTYPE);
RTC_DS3231 clock;

DeviceState currentState = SYS_DISABLED, previousState;

char lcdContent[LCD_LEN], errorMsg[LCD_LEN];
const char* stateLabels[4] = {"(DISABLED)", "IDLE", "ERROR", "RUNNING"};
const unsigned char ledMap[4] = {LED_YELLOW, LED_GREEN, LED_RED, LED_BLUE};

const unsigned int TEMP_THRESHOLD = 42;
const unsigned int WATER_THRESHOLD = 100;
unsigned int waterReading = 0;

void systemSetup();
void readSensors();
void updateLCD();
void changeState(DeviceState);
void handleFan();
void checkVentControl();
void handleSerialReport(DateTime);
void configureADC();
unsigned int analogReadChannel(unsigned char);
void uartInit(unsigned long);
void uartPrint(const char*, int);
void uartSend(unsigned char);

void setup(){
  display.begin(16, 2);
  climateSensor.begin();
  clock.begin();
  clock.adjust(DateTime(F(__DATE__), F(__TIME__)));
  display.clear();
  ventStepper.setSpeed(2);
  systemSetup();
  configureADC();
  uartInit(19200);
}

void loop(){
  DateTime now = clock.now();
  readSensors();
  updateLCD();
  handleFan();
  checkVentControl();

  if(previousState != currentState){
    handleSerialReport(now);
    previousState = currentState;
  }
}

void readSensors(){
  if(currentState != SYS_DISABLED){
    waterReading = analogReadChannel(0);

    if(currentState == SYS_IDLE || currentState == SYS_RUNNING){
      snprintf(lcdContent, LCD_LEN, "H:%d T:%dF", (int)climateSensor.readHumidity(), (int)climateSensor.readTemperature(true));
    }

    if(currentState == SYS_IDLE && climateSensor.readTemperature(true) >= TEMP_THRESHOLD){
      changeState(SYS_RUNNING);
    }
    else if(currentState == SYS_RUNNING && climateSensor.readTemperature(true) < TEMP_THRESHOLD){
      changeState(SYS_IDLE);
    }

    if(waterReading < WATER_THRESHOLD){
      changeState(SYS_ERROR);
    }
  }
}

void updateLCD(){
  display.setCursor(0, 0);
  if(currentState == SYS_ERROR){
    display.print(errorMsg);
  }
  else{
    display.print(lcdContent);
  }

  display.setCursor(0, 1);
  display.print(stateLabels[currentState]);

  *PORT_C = ledMap[currentState];
}

void changeState(DeviceState newState){
  if(newState == SYS_ERROR){
    snprintf(errorMsg, LCD_LEN, "Low water!");
  }
  currentState = newState;
}

void handleFan(){
  if(currentState == SYS_RUNNING){
    *PORT_B |= FAN_MASK;
  }
  else{
    *PORT_B &= ~FAN_MASK;
  }
}

void checkVentControl(){
  if(*PIN_B & BUTTON_CTRL && currentState != SYS_DISABLED){
    ventStepper.step(1);
    uartPrint("\nVENT MOVED\n", 12);
  }
}

void handleSerialReport(DateTime time){
  char buf[64];
  snprintf(buf, sizeof(buf), "\nSTATE: %s -> %s\n", stateLabels[previousState], stateLabels[currentState]);
  uartPrint(buf, strlen(buf));
  snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d  Date: %d/%d/%d\n",
           time.hour(), time.minute(), time.second(), time.day(), time.month(), time.year());
  uartPrint(buf, strlen(buf));
}

void systemSetup(){
  *DDR_C |= (LED_GREEN | LED_YELLOW | LED_RED | LED_BLUE);
  *DDR_B |= FAN_MASK;

  *PORT_B |= (BUTTON_START | BUTTON_RESET | BUTTON_CTRL);
  *DDR_B &= ~(BUTTON_START | BUTTON_RESET | BUTTON_CTRL);

  *myPCICR |= 0x01;
  *myPCMSK0 |= PCI_MASK;
}

void configureADC(){
  *my_ADCSRA |= 0x80;
  *my_ADCSRA &= ~0x20;
  *my_ADCSRB &= ~0x08;
  *my_ADCSRB &= ~0x07;
  *my_ADMUX  &= 0x7F;
  *my_ADMUX  |= 0x40;
  *my_ADMUX  &= ~0x20;
  *my_ADMUX  &= 0xE0;
}

unsigned int analogReadChannel(unsigned char channel){
  *my_ADMUX &= 0xE0;
  *my_ADCSRB &= ~0x08;

  if(channel > 7){
    channel -= 8;
    *my_ADCSRB |= 0x08;
  }

  *my_ADMUX |= channel;
  *my_ADCSRA |= 0x40;
  while (*my_ADCSRA & 0x40);
  return *my_ADC_DATA;
}

void uartInit(unsigned long baud){
  unsigned long cpu_freq = 16000000;
  unsigned int ubrr = (cpu_freq / 16 / baud) - 1;
  *myUCSR0A = 0x20;
  *myUCSR0B = 0x18;
  *myUSCR0C = 0x06;
  *myUBRR_0 = ubrr;
}

void uartSend(unsigned char c){
  while (!(*myUCSR0A & TBE));
  *myUDR_0 = c;
}

void uartPrint(const char* str, int len){
  for(int i = 0; i < len && str[i] != '\0'; i++){
    uartSend(str[i]);
  }
}

ISR(PCINT0_vect){
  if(*PIN_B & BUTTON_RESET && currentState == SYS_ERROR){
    changeState(SYS_IDLE);
  }
  else if (*PIN_B & BUTTON_START){
    if(currentState == SYS_DISABLED){
      changeState(SYS_IDLE);
    }
    else{
      changeState(SYS_DISABLED);
    }
  }
}
