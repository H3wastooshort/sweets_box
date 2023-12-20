/*
   A sweets box that won't open (and scream) if you take to much out of it
   I actually tried to comment this code (that didn't work out)
*/

#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <HX711.h>  //https://github.com/RobTillaart/HX711
#include <Servo.h>
#include <Wire.h>
#include <TimeLib.h>
#include <DS1307RTC.h>
#include <AT24CX.h>  //https://github.com/cyberp/AT24Cx


#define RESET_HOUR 4  //Hour to reset the weight on (4 should cover most nightowls as well :) )

#define LOCK_IN_DELAY 4000  //how long to wait from detecting too little weight to locking the lid

#define DEBOUNCE_DELAY 75  //delay in ms
#define BTN_1_PIN 2        //Up button
#define BTN_2_PIN 3        //Down button

#define SCALE_DATA_PIN 11
#define SCALE_CLOCK_PIN 12
#define SCALE_NOISE_THRESH 0.5  //wait until scale noise settled below this value
#define SCALE_SAMPLE_AMOUNT 3   //how many times to sample the scale (3 times to 15 times)
#define SCALE_UPDATE_SLOW 1000  // update interval for when the lid is closed

#define SERVO_PIN 10
#define SERVO_LID_OPEN 180  //servo open position in degrees
#define SERVO_LID_CLOSE 0   //servo closed position in degrees
#define SERVO_ON_TIME 2000  //time to drive the servo for in milliseconds

#define LID_SENSOR_PIN A0
#define LID_SENSOR_NO_INVERT false  //true for reed switch between GND and A0, false for hall effect boards

#define BUZZER_PIN A1
#define RED_LIGHTING_PIN A2
#define GREEN_LIGHTING_PIN A3

#define EEPROM_MAGIC_NUMBER 69

#define SCALE_OFFSET_EEPROM_ADDR 0
#define SCALE_SCALE_EEPROM_ADDR 1
#define MAX_WITHDRAW_EEPROM_ADDR 2
#define DAY_START_WEIGHT_DAY_EEPROM_ADDR 3
#define DAY_START_WEIGHT_EEPROM_ADDR 4
#define BUZZER_ENABLED_EEPROM_ADDR 5
#define LOCK_IN_EEPROM_ADDR 6

//i should make this a struct
//                                  0,            1,           2,                    3,                    4,                5,              6
//                                  scale_offset, scale_scale, max_withdraw_per_day, day_start_weight_day, day_start_weight, buzzer_enabled, lock_in
/*const uint8_t eeprom_var_sizes[] = {sizeof(long), sizeof(float), sizeof(uint16_t), sizeof(float) , sizeof(uint8_t), sizeof(bool), sizeof(bool)};

  uint16_t getVarAddrEEPROM(uint8_t var_num) {
  int16_t var_addr = 0;
  for (uint8_t var_i = 0; var_i < var_num; var_i++) {
    var_addr += eeprom_var_sizes[var_i];
  }

  return var_addr; //fun fact: i forgot to add this, causing all config to corrupt each other, and it took me a good 30 minutes to debug this
  }*/

uint16_t getVarAddrEEPROM(uint8_t var_num) {  //i give up. im going the stupid way
  int16_t var_addr = 0;
  for (uint8_t var_i = 0; var_i < var_num; var_i++) {  //fuck it. always add 4 bytes. the EEPROM is big enough. im going insane
    var_addr += 4;
  }

  return var_addr;
}

//boring library stuff
//                RS E D4 D5 D6 D7
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);
HX711 scale;
Servo lid_lock;
AT24C32 log_mem;

//Vars for withdraw (all weights in gram, i hate imperial units)
float current_weight = 0;
float day_start_weight = 0;
uint16_t max_withdraw_per_day = 100;
bool buzzer_enabled = true;
tmElements_t current_tm;
static uint8_t last_day = 0;  //for resetting on new day
bool lock_in = false;
bool open_servo = true;   //stops setServo() from constantly being called when open
bool close_servo = true;  //stops setServo() from constantly being called when open

void logStatistics() {  //Function to write log to EEPROM
  Wire.beginTransmission(0x50);
  if (Wire.endTransmission() != 0) {  //check if EEPROM present
    Serial.println(F("Log EEPROM missing. This reset is not logged."));
    return;
  }

  Serial.println(F("Logging todays data. Retrive all log data by sending 'L' via Serial."));


  //first 2 bytes are for current writing index
  unsigned int current_write_pos = log_mem.readInt(0);  //read current writing pos
  if (current_write_pos >= 4096) {                      //if at end of eeprom
    current_write_pos = 2;                              //start writing from the beginnig again
  }


  float withdrawn_today = (current_weight - day_start_weight) * -1;

  Serial.print(F("Logging to address: "));
  Serial.println(current_write_pos);

  //write time
  log_mem.write(current_write_pos, current_tm.Year);
  current_write_pos += 1;
  log_mem.write(current_write_pos, current_tm.Month);
  current_write_pos += 1;
  log_mem.write(current_write_pos, current_tm.Day);
  current_write_pos += 1;

  //write taken
  log_mem.writeFloat(current_write_pos, withdrawn_today);
  current_write_pos += 4;
  Serial.print(F("Taken today logged: "));
  Serial.println(withdrawn_today);

  //write weight
  log_mem.writeFloat(current_write_pos, current_weight);
  current_write_pos += 4;
  Serial.print(F("Weight logged: "));
  Serial.println(current_weight);

  //write index
  log_mem.writeInt(0, current_write_pos);  //save the next writable address to address 0
  Serial.print(F("Next address will be: "));
  Serial.println(current_write_pos);

  Serial.println(F("Logged todays data."));
}

//servo control
uint32_t servo_pos_set_millis = 0xFFFFFFFF;
void setServo(int16_t servo_angle) {
  lid_lock.attach(SERVO_PIN);
  lid_lock.write(servo_angle);
  servo_pos_set_millis = millis();
  Serial.print(F("Servo set to "));
  Serial.println(servo_angle);
}

//Vars for menu stuff
bool disable_button_handlers = false;
int8_t main_screen = 0;        //which standby screen to show -2 = in some menu
int8_t setting_selected = -2;  //wich setting is selected  -2 = none->standby screen
bool editing_setting = false;  //set true by interrupt when a setting function should be executed.

//millis() when the buttons were pressed last time (if 0, buttons are up)
uint32_t btn_1_down_millis = 0;
uint32_t btn_2_down_millis = 0;

//menus and stuff

void clearSerialInput() {
  while (Serial.available()) Serial.read();
}

void fullDispMsg(String line1, String line2) {  //Simple function for flashing sth on the display
  lcd.clear();
  lcd.home();
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void waitTilStable() {
  // read until stable
  float w1, w2;
  w1 = scale.get_units(15);
  delay(100);
  w2 = scale.get_units(1);
  while (abs(w1 - w2) > SCALE_NOISE_THRESH) {
    w1 = w2;
    w2 = scale.get_units();
    lcd.setCursor(12, 0);
    lcd.print(F(" "));
    lcd.print((uint8_t)abs(w1 - w2));
    lcd.print(F(">"));
    lcd.print(SCALE_NOISE_THRESH);
    delay(100);
  }
}

void setting_exit() {
  setting_selected = -2;
  main_screen = 0;
  editing_setting = false;
  EEPROM.put(getVarAddrEEPROM(BUZZER_ENABLED_EEPROM_ADDR), buzzer_enabled);
}

void setting_cal() {
  clearSerialInput();
  Serial.println(F("Calibration started..."));
  scale.set_average_mode();  //medavg is better but can only do up to 15 readings
  fullDispMsg(F("Put 0g in box"), F("then press btn"));
  while (digitalRead(BTN_1_PIN) and digitalRead(BTN_2_PIN) and !Serial.available()) {}
  clearSerialInput();

  fullDispMsg(F("Calibrating..."), F("DO NOT TOUCH"));
  waitTilStable();
  delay(1000);
  scale.tare(250);
  long scale_offset = scale.get_offset();
  EEPROM.put(getVarAddrEEPROM(SCALE_OFFSET_EEPROM_ADDR), scale_offset);

  fullDispMsg(F("Put 500g in box"), F("then press btn"));
  while (digitalRead(BTN_1_PIN) and digitalRead(BTN_2_PIN) and !Serial.available()) {}
  clearSerialInput();

  fullDispMsg(F("Calibrating..."), F("DO NOT TOUCH"));
  delay(1000);
  waitTilStable();
  scale.calibrate_scale(500, 250);  //lets hope this fits in ram
  float scale_scale = scale.get_scale();
  EEPROM.put(getVarAddrEEPROM(SCALE_SCALE_EEPROM_ADDR), scale_scale);

  EEPROM.write(EEPROM.length() - 1, EEPROM_MAGIC_NUMBER);  //Write magic number for scale cal

  Serial.println(F("Calibration done"));
  Serial.print(F("New scale offset: "));
  Serial.println(scale_offset);
  Serial.print(F("New scale scale: "));
  Serial.println(scale_scale);

  fullDispMsg(F("======DONE======"), F(""));
  delay(1000);
  scale.set_medavg_mode();
}

//TODO: allow option to keep the current taken_today after tare
void setting_tare() {
  setServo(SERVO_LID_OPEN);  //open lid and wait for fill

  fullDispMsg(F("Fill up box"), F("then close box"));
  while (digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) {}

  fullDispMsg(F("take away hands"), F("then press btn"));
  while (digitalRead(BTN_1_PIN) and digitalRead(BTN_2_PIN)) {}

  fullDispMsg(F("Resetting..."), F("DO NOT TOUCH"));
  delay(1000);

  logStatistics();

  day_start_weight = scale.get_units(15);
  last_day = current_tm.Day - 1;  //reset functions in manageLid()
  lock_in = false;

  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_DAY_EEPROM_ADDR), (uint8_t)current_tm.Day);  //Set starting weight day to today
  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), current_weight);               //Set starting weight day to current weight on scale
  EEPROM.put(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);

  fullDispMsg(F("======DONE======"), F(""));
  delay(1000);
}

void setting_buzzer() {
  buzzer_enabled = !buzzer_enabled;
  lcd.setCursor(0, 1);
  if (buzzer_enabled) lcd.print(F("ENABLED         "));
  else lcd.print(F("DISABLED        "));
}

uint8_t getButtonBlocking() {  //Block until button(s) pressed
gbb_debounce:
  while (digitalRead(BTN_1_PIN) and digitalRead(BTN_2_PIN)) {}  //wait for button press
  delay(200);
  if (digitalRead(BTN_1_PIN) and digitalRead(BTN_2_PIN)) goto gbb_debounce;  //If it was just a fluke, continue waiting

  uint8_t button_state = 0;

  if (!digitalRead(BTN_1_PIN)) button_state += 1;
  if (!digitalRead(BTN_2_PIN)) button_state += 2;

  /*
    0 = no button (should not happen)
    1 = btn 1
    2 = btn 2
    3 = both buttons
  */

  digitalWrite(LED_BUILTIN, HIGH);
  tone(BUZZER_PIN, 1000, 10);
  while (!digitalRead(BTN_1_PIN) or !digitalRead(BTN_2_PIN)) {}  //Wait until buttons are released
  digitalWrite(LED_BUILTIN, LOW);

  return button_state;
}

void setting_tare_keep() {
  float old_taken;

  fullDispMsg(F("Close Box"), F(""));
  while (digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) {}
  delay(250);
  setServo(SERVO_LID_CLOSE);

  fullDispMsg(F("take away hands"), F("then press btn"));
  getButtonBlocking();  //just discard button value

  fullDispMsg(F("Please wait..."), F("DO NOT TOUCH"));
  old_taken = (scale.get_units(15) - day_start_weight) * -1;

  setServo(SERVO_LID_OPEN);
  fullDispMsg(F("fill up box"), F("then press btn"));
  getButtonBlocking();  //just discard button value

  fullDispMsg(F("Close Box"), F(""));
  while (digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) {}
  delay(250);
  setServo(SERVO_LID_CLOSE);
  float new_weight = scale.get_units(15);

  day_start_weight = new_weight + old_taken;
  //last_day = current_tm.Day - 1; //reset functions in manageLid()
  //lock_in = (day_start_weight > max_withdraw_per_day);

  //EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_DAY_EEPROM_ADDR), (uint8_t)current_tm.Day); //Set starting weight day to today
  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), day_start_weight);  //Set starting weight day to current weight on scale
  //EEPROM.put(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);

  //we messed with the servo so manageLimiting() must reset it
  open_servo = true;
  close_servo = true;

  fullDispMsg(F("======DONE======"), F(""));
  delay(1000);
}

void setting_add_ootb() {
  //declare all vars before goto label
  float sweet_weight, new_weight, old_weight;
  String sweets_weight_str;

  fullDispMsg(F("Close Box"), F(""));
  while (digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) {}
  delay(250);
  setServo(SERVO_LID_CLOSE);

another_ootb_sweet:
  fullDispMsg(F("take away hands"), F("and clear top"));
  getButtonBlocking();  //just discard button value

  fullDispMsg(F("Please wait..."), F("DO NOT TOUCH"));
  old_weight = scale.get_units(15);

  fullDispMsg(F("Lay OOTB sweet"), F("on top of box"));
  getButtonBlocking();  //just discard button value

  fullDispMsg(F("Please wait..."), F("DO NOT TOUCH"));
  new_weight = scale.get_units(15);

  sweet_weight = new_weight - old_weight;  //calc weight of sweet
  day_start_weight += sweet_weight;

  sweets_weight_str = F("Weight was ");
  sweets_weight_str += sweet_weight;
  sweets_weight_str += "g";

  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), day_start_weight);

  fullDispMsg(F("Sweet registered"), sweets_weight_str);
  delay(2000);

  fullDispMsg(F("UP->another"), F("DOWN->quit"));
  if (getButtonBlocking() == 1) goto another_ootb_sweet;

  //we messed with the servo so manageLimiting() must reset it
  open_servo = true;
  close_servo = true;
  setServo(SERVO_LID_OPEN);

  fullDispMsg(F("======DONE======"), F(""));
  delay(1000);
}

void setting_limit() {
  while (true) {
    lcd.setCursor(0, 1);
    lcd.print(F("max "));
    lcd.print(max_withdraw_per_day);
    lcd.print("g daily             ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        max_withdraw_per_day += 10;
        break;
      case 2:
        max_withdraw_per_day -= 10;
        break;
      case 3:
        EEPROM.put(getVarAddrEEPROM(MAX_WITHDRAW_EEPROM_ADDR), max_withdraw_per_day);
        return;
    }
  }
}

void setting_date() {  //note: this function is for setting the date and time, not for asking the arduino out
  bool loop_break = true;

  while (loop_break) {
    lcd.setCursor(0, 0);
    lcd.print(F("SET HOUR        "));
    lcd.setCursor(0, 1);
    lcd.print(current_tm.Hour);
    lcd.print("            ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        current_tm.Hour++;
        break;
      case 2:
        current_tm.Hour--;
        break;
      case 3:
        loop_break = false;
    }

    if (current_tm.Hour > 23) current_tm.Hour = 0;
  }
  loop_break = true;
  while (loop_break) {
    lcd.setCursor(0, 0);
    lcd.print(F("SET MINUTE     "));
    lcd.setCursor(0, 1);
    lcd.print(current_tm.Minute);
    lcd.print("            ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        current_tm.Minute++;
        break;
      case 2:
        current_tm.Minute--;
        break;
      case 3:
        loop_break = false;
    }

    if (current_tm.Minute > 59) current_tm.Minute = 0;
  }
  loop_break = true;
  while (loop_break) {
    lcd.setCursor(0, 0);
    lcd.print(F("SET DAY        "));
    lcd.setCursor(0, 1);
    lcd.print(current_tm.Day);
    lcd.print("            ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        current_tm.Day++;
        break;
      case 2:
        current_tm.Day--;
        break;
      case 3:
        loop_break = false;
    }

    if (current_tm.Day > 31 or current_tm.Day < 1) current_tm.Day = 0;
  }
  loop_break = true;
  while (loop_break) {
    lcd.setCursor(0, 0);
    lcd.print(F("SET MONTH      "));
    lcd.setCursor(0, 1);
    lcd.print(current_tm.Month);
    lcd.print("            ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        current_tm.Month++;
        break;
      case 2:
        current_tm.Month--;
        break;
      case 3:
        loop_break = false;
    }

    if (current_tm.Month > 12 or current_tm.Month < 1) current_tm.Month = 0;
  }
  loop_break = true;
  int calender_year = 2022;
  while (loop_break) {
    lcd.setCursor(0, 0);
    lcd.print(F("SET YEAR       "));
    lcd.setCursor(0, 1);
    lcd.print(calender_year);
    lcd.print("            ");  //clear line without flicker

    switch (getButtonBlocking()) {
      case 1:
        calender_year++;
        break;
      case 2:
        calender_year--;
        break;
      case 3:
        loop_break = false;
    }

    //fuck it, you can set the year to 9999 or 0
  }
  current_tm.Year = CalendarYrToTm(calender_year);
  current_tm.Second = 0;
  RTC.write(current_tm);
  fullDispMsg(F("RTC SET"), F(""));
}

void setting_reset() {         //Reset everyting to sensible values
  max_withdraw_per_day = 100;  //g
  buzzer_enabled = true;
  EEPROM.put(getVarAddrEEPROM(MAX_WITHDRAW_EEPROM_ADDR), max_withdraw_per_day);
  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_DAY_EEPROM_ADDR), current_weight);       //Set starting weight day to current weight on scale
  EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), (uint8_t)current_tm.Day);  //Set starting weight day to today
  EEPROM.put(getVarAddrEEPROM(BUZZER_ENABLED_EEPROM_ADDR), buzzer_enabled);

  fullDispMsg(F("Reset settings"), F("to defaults."));
  delay(1000);
}

//Setting names and functions
#define N_OF_SETTINGS 9
const char* settings_names[N_OF_SETTINGS] = { "EXIT SETTINGS", "Add Out-of-Box", "Tare&KeepTaken", "Tare&ResetTaken", "Overdraft Buzzer", "Maximum per day", "Set current Date", "Calibrate Scale", "RESET SETTINGS" };
void (*settings_functions[N_OF_SETTINGS])() = { setting_exit, setting_add_ootb, setting_tare_keep, setting_tare, setting_buzzer, setting_limit, setting_date, setting_cal, setting_reset };

#define N_OF_MAIN_SCREENS 2


int8_t last_setting = -2;  //makes the settings menu not constantly redraw everything
void updateDisplay() {
  if (editing_setting) {
    disable_button_handlers = true;             //the functions handle this themself
    (*settings_functions[setting_selected])();  //hand off to the correct settings function
    editing_setting = false;                    //The function is done. This is set back to false to not run it again
    disable_button_handlers = false;            //button contol menu again

    //cause redraw
    last_setting = -2;
  }

  if (main_screen >= 0 and setting_selected == -2) {  //if the thing is supposed to show a homescreen
    static uint32_t main_screen_refresh_last_millis = 0;
    if (millis() - main_screen_refresh_last_millis > 1000) {  // Update every 1000ms
      //homescreen routine
      float withdrawn_today = (current_weight - day_start_weight) * -1;
      Serial.print(F("Current weight:"));
      Serial.println(current_weight);
      Serial.print(F("Withdrawn today:"));
      Serial.println(withdrawn_today);

      switch (main_screen) {
        default:
          main_screen = 0;
          Serial.println(F("MENU SYSTEM FAULT WHILE DRAWING DISPLAY"));  //If you get this, something is extremely fucked
          //continues with case 0

        case 0:  //scale
          lcd.clear();
          lcd.home();
          lcd.print(F("Took "));
          lcd.print((int16_t)withdrawn_today);
          lcd.print(F("g/"));
          lcd.print(max_withdraw_per_day);
          lcd.print(F("g"));
          if (withdrawn_today > max_withdraw_per_day) {
            lcd.print(F("!"));
          }
          if (lock_in) {
            lcd.print(F("!"));
          }

          lcd.setCursor(0, 1);
          lcd.print(F("Weight: "));
          lcd.print(current_weight);
          lcd.print(F("g"));
          break;

        case 1:  //clock
          lcd.clear();
          lcd.home();

          //These buffers will be filled by sprintf()
          char linebuf1[16];
          char linebuf2[16];

          //sprintf() does not like the F() macros so i put them in strings and give it the strings as c_str()
          //the padding is to center the stuff
          //crashes the 2nd time the clock is displayed tho
          //String line1format = F("    %02u:%02u:%02u    ");
          //String line2format = F("   %02u.%02u.%04u   ");
          //sprintf(linebuf1, line1format.c_str(), current_tm.Hour, current_tm.Minute, current_tm.Second);
          //sprintf(linebuf2, line2format.c_str(), current_tm.Day, current_tm.Month, tmYearToCalendar(current_tm.Year));

          //guess i will have to waste ram :(
          sprintf(linebuf1, "    %02u:%02u:%02u    ", current_tm.Hour, current_tm.Minute, current_tm.Second);
          sprintf(linebuf2, "   %02u.%02u.%04u   ", current_tm.Day, current_tm.Month, tmYearToCalendar(current_tm.Year));

          fullDispMsg(linebuf1, linebuf2);  //Display buffers
          break;
      }

      main_screen_refresh_last_millis = millis();
      return;
    }
  }

  if (main_screen == -2 and setting_selected >= 0) {  //Settings display routine
    if (setting_selected != last_setting) {
      last_setting = setting_selected;

      String setting_state;
      switch (setting_selected) {
        default:
          setting_selected = 0;
          Serial.println(F("MENU SYSTEM FAULT WHILE DRAWING DISPLAY!!"));  //If you get this, something is extremely fucked
          //continues with case 0

        case 0:
          setting_state = F("(Click both btn)");
          break;

        case 1:
          setting_state = F("sweet to TakenTD");
          break;

        case 2:
          setting_state = F("run BEFORE refill");
          break;

        case 3:
          setting_state = F("TakenToday -> 0g");
          break;

        case 4:
          if (buzzer_enabled) setting_state = F("ENABLED");
          else setting_state = F("DISABLED");
          break;

        case 5:
          setting_state += max_withdraw_per_day;
          setting_state += "g";
          break;

        case 6:
          break;

        case 7:
          setting_state = F("needs 500g ref");
          break;

        case 8:
          setting_state = F("to defaults?");
          break;
      }

      fullDispMsg(settings_names[setting_selected], setting_state);  //Show setting name and current state
    }
  }
}


//Button handlers

void changeMenuPage(bool increase_page) {
  if (main_screen >= 0 and setting_selected == -2) {  //if on homescreen
    if (increase_page) {
      main_screen++;

      if (main_screen >= N_OF_MAIN_SCREENS) {  //cant go higher than N_OF_MAIN_SCREENS-1
        main_screen = N_OF_MAIN_SCREENS - 1;
      }
    } else {
      main_screen--;

      if (main_screen < 0 and main_screen != -2) {  //cant go lower than 0
        main_screen = 0;
      }
    }
  }

  else if (setting_selected >= 0 and main_screen == -2) {  //if in settings menu
    if (increase_page) {
      setting_selected++;

      if (setting_selected >= N_OF_SETTINGS) {  //cant go higher than N_OF_SETTINGS-1
        setting_selected = N_OF_SETTINGS - 1;
      }
    } else {
      setting_selected--;

      if (setting_selected < 0 and setting_selected != -2) {  //cant go lower than 0
        setting_selected = 0;
      }
    }
  }

  else {                                      //If none of the conditions are met
    Serial.println(F("MENU SYSTEM FAULT!"));  //If you get this, something is VERY FUCKED
  }
}


void handleButtons() {
  if (millis() - btn_1_down_millis > DEBOUNCE_DELAY and btn_1_down_millis > 0) {  //if pressed long enough ago
    if (!digitalRead(BTN_1_PIN)) {                                                //and it is still held
      if (btn_2_down_millis > 0) {
        handleBothButtons();
        btn_2_down_millis = 0;
      } else
        handleButton1();
    }
    //afterwards, or if it was just bouncing
    btn_1_down_millis = 0;
  }

  if (millis() - btn_2_down_millis > DEBOUNCE_DELAY and btn_2_down_millis > 0) {  //if pressed long enough ago
    if (!digitalRead(BTN_2_PIN)) {                                                //and it is still held
      if (btn_1_down_millis > 0) {
        handleBothButtons();
        btn_1_down_millis = 0;
      } else
        handleButton2();
    }
    //afterwards, or if it was just bouncing
    btn_2_down_millis = 0;
  }
}

void handleButton1() {
  if (disable_button_handlers) return;  //Return if other button was first

  tone(BUZZER_PIN, 1600, 10);

  Serial.println(F("Button 1 pressed"));

  //regular press
  changeMenuPage(true);
}

void handleButton2() {
  if (disable_button_handlers) return;  //Return if other button was first

  tone(BUZZER_PIN, 1500, 10);

  Serial.println(F("Button 2 pressed"));

  //regular press
  changeMenuPage(false);
}

void handleBothButtons() {
  tone(BUZZER_PIN, 2000, 10);

  Serial.println(F("Both Buttons pressed"));

  if (setting_selected == -2) {  //if no settings, enter settings and return
    setting_selected = 0;
    main_screen = -2;
    updateDisplay();
  } else {
    editing_setting = true;  //This will be picked up by the display update routine
  }
}

void handleButton1Down() {
  btn_1_down_millis = millis();
}
void handleButton2Down() {
  btn_2_down_millis = millis();
}

void readDevices() {
  RTC.read(current_tm);

  auto reading_start_millis = millis();

  static uint32_t scale_update_last_millis = 0;
  if (millis() - scale_update_last_millis > SCALE_UPDATE_SLOW /*each SCALE_UPDATE_SLOW milliseconds*/ or (digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) /*if lid open*/) {
    current_weight = scale.get_units((digitalRead(LID_SENSOR_PIN) != LID_SENSOR_NO_INVERT) ? SCALE_SAMPLE_AMOUNT : min(SCALE_SAMPLE_AMOUNT * 2, 15));  //if lid closed, take twice as many samples but at maximum 15
    scale_update_last_millis = millis();

    /*Serial.print(F("Reading time: "));
    Serial.print(millis() - reading_start_millis);
    Serial.println(F("ms"));*/
  }
}

void manageLimiting() {
  float withdrawn_today = (current_weight - day_start_weight) * -1;

  if (last_day != current_tm.Day and current_tm.Hour >= RESET_HOUR) {  //if its n new day and its after RESET_HOUR AM
    logStatistics();                                                   //log this days data to the EEPROM of the RTC module (if present)

    setServo(SERVO_LID_OPEN);                                                        //Unlock the lid
    lock_in = false;                                                                 //allow the thing to open again
    last_day = current_tm.Day;                                                       //reset sw day to today
    day_start_weight = scale.get_units(15);                                          //reset sw to todays sw
    EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_DAY_EEPROM_ADDR), current_tm.Day);  //reset sw day in EEPROM
    EEPROM.put(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), day_start_weight);    // reset sw to current weight in EEPROM
    EEPROM.put(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);                      // reset lock_in
    noTone(BUZZER_PIN);
    digitalWrite(GREEN_LIGHTING_PIN, HIGH);  //Green lighting
    digitalWrite(RED_LIGHTING_PIN, LOW);
    //Serial.println(F("Reset day starting weight because its past 4:00AM on a new day."));
    Serial.println(F("DSW reset"));
  }

  if (lock_in) {
    digitalWrite(GREEN_LIGHTING_PIN, LOW);  //Red lighting
    digitalWrite(RED_LIGHTING_PIN, HIGH);
    if (close_servo) {
      setServo(SERVO_LID_CLOSE);
      close_servo = false;
    }
    return;  //no need to execute the rest
  }
  close_servo = true;  //reset if lock_in gone

  static uint32_t lock_in_delay_last_millis = 0xFFFFFFFF;
  static bool reset_lock_in_delay = false;
  if (withdrawn_today > max_withdraw_per_day) {
    open_servo = true;
    if (digitalRead(LID_SENSOR_PIN) == LID_SENSOR_NO_INVERT) {
      if (reset_lock_in_delay) {
        lock_in_delay_last_millis = millis();
        reset_lock_in_delay = false;
      }
      if (millis() - lock_in_delay_last_millis > LOCK_IN_DELAY) {
        lock_in = true;                                              //once closed, stay closed, even if sensor is interrupted shortly
        EEPROM.put(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);  //write lockin to eeprom
        setServo(SERVO_LID_CLOSE);
        noTone(BUZZER_PIN);  //Lid closed. stop beeping

        digitalWrite(GREEN_LIGHTING_PIN, LOW);  //Red lighting
        digitalWrite(RED_LIGHTING_PIN, HIGH);
      } else {  //warn that locking is imminent
        if (buzzer_enabled) tone(BUZZER_PIN, constrain(map((millis() - lock_in_delay_last_millis), 0, LOCK_IN_DELAY, 750, 250), 250, 750));
        digitalWrite(GREEN_LIGHTING_PIN, HIGH);  //Yellow lighting
        digitalWrite(RED_LIGHTING_PIN, HIGH);
      }
    } else {
      static uint32_t last_beep_millis = 0;
      if (buzzer_enabled)
        if (millis() - last_beep_millis > 250) {
          tone(BUZZER_PIN, 1000, 50);
          last_beep_millis = millis();
        }                                      //Annoy the human until they close the lid or put the stuff back
      digitalWrite(GREEN_LIGHTING_PIN, HIGH);  //Yellow lighting
      digitalWrite(RED_LIGHTING_PIN, HIGH);
      reset_lock_in_delay = true;
    }
  } else {  //If the stuff was put back before closing the lid, resume normal operation
    noTone(BUZZER_PIN);
    if (open_servo) {
      setServo(SERVO_LID_OPEN);
      open_servo = false;
    }
    reset_lock_in_delay = true;
    digitalWrite(GREEN_LIGHTING_PIN, HIGH);  //Green lighting
    digitalWrite(RED_LIGHTING_PIN, LOW);
  }

  //turn off servo after delay so i does not make annoying sounds (the load cell reading function probably turns off interrupts)
  if ((millis() - servo_pos_set_millis > SERVO_ON_TIME) and servo_pos_set_millis != 0xFFFFFFFF) {
    lid_lock.detach();
    servo_pos_set_millis = 0xFFFFFFFF;
    //Serial.println(F("Turned off servo after timeout."));
  }
}

void handleSerialControl() {  //used for debugging, starts setting functions because i still have issues with the menu buttons
  if (Serial.available()) {
    uint8_t controlCharacter = Serial.read();

    if (controlCharacter == 'C') {  //calibrate
      disable_button_handlers = true;
      setting_cal();
      disable_button_handlers = false;
    } else if (controlCharacter == 'T') {  //tare starting weight
      disable_button_handlers = true;
      setting_tare();
      disable_button_handlers = false;
    } else if (controlCharacter == 'R') {  //reset to sensible config
      disable_button_handlers = true;
      setting_reset();
      disable_button_handlers = false;
      Serial.print(F("Reset EEPROM config to sensible values."));
    } else if (controlCharacter == 'I') {  //INIT log EEPROM
      Wire.beginTransmission(0x50);
      if (Wire.endTransmission() != 0) {  //check if EEPROM present
        Serial.println(F("Log EEPROM missing."));
        return;
      }

      Serial.println(F("Initializing log EEPROM."));

      for (uint16_t addr = 0; addr < 4096; addr++) {  //clear entire EEPROM
        log_mem.write(addr, 0);
        if (addr % 16 == 0) {
          Serial.write('\r');
          Serial.print(addr);
          Serial.write(' ');
        }
      }
      log_mem.writeInt(2, 0);  //reset write index
      Serial.write('\r');

      Serial.println(F("Initialized."));

    } else if (controlCharacter == 'L') {  //print out log
      Wire.beginTransmission(0x50);
      if (Wire.endTransmission() != 0) {  //check if EEPROM present
        Serial.println(F("Log EEPROM missing."));
        return;
      }

      Serial.println(F("All log data in the EEPROM:\n"));
      Serial.flush();

      for (uint16_t log_addr = 2; log_addr < 4096;) {  //run trough all addresses
        //print time
        Serial.print(F("Date: "));
        Serial.print(tmYearToCalendar(log_mem.read(log_addr)));
        log_addr += 1;
        Serial.write('-');
        Serial.print(log_mem.read(log_addr));
        log_addr += 1;
        Serial.write('-');
        Serial.print(log_mem.read(log_addr));
        Serial.println();
        log_addr += 1;
        Serial.flush();

        //print taken
        Serial.print(F("Taken: "));
        Serial.println(log_mem.readFloat(log_addr));
        Serial.flush();
        log_addr += sizeof(float);

        //print weight
        Serial.print(F("Weight: "));
        Serial.println(log_mem.readFloat(log_addr));
        Serial.flush();
        log_addr += sizeof(float);

        Serial.println(F("----"));
        Serial.flush();
      }

      delay(2500);
    } else if (controlCharacter == 'U') {                          //unlock box
      lock_in = false;                                             //unlock
      EEPROM.put(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);  //write lockin to eeprom
      Serial.println(F("Unlocked Box."));
    } else if (controlCharacter == 'D') {  //dump all EEPROMs
      Serial.println(F("==Internal EEPROM=="));
      for (uint16_t addr = 0; addr < EEPROM.length(); addr++) {
        char hexbuf[10];
        sprintf(hexbuf, "%02x ", EEPROM.read(addr));
        Serial.print(hexbuf);

        if (addr % 32 == 0 and addr >= 32) Serial.println();  //add newline every 32 reads
      }
      Serial.println(F("\n==EOF==\n"));

      Wire.beginTransmission(0x50);
      if (Wire.endTransmission() != 0) {  //check if RTC EEPROM present
        return;
      }
      Serial.println(F("==RTC EEPROM=="));
      for (uint16_t addr = 0; addr < 4096; addr++) {
        char hexbuf[10];
        sprintf(hexbuf, "%02x ", log_mem.read(addr));
        Serial.print(hexbuf);

        if (addr % 32 == 0 and addr >= 32) Serial.println();  //add newline every 32 reads
      }
      Serial.println(F("\n==EOF==\n"));
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(GREEN_LIGHTING_PIN, OUTPUT);
  pinMode(RED_LIGHTING_PIN, OUTPUT);

  digitalWrite(GREEN_LIGHTING_PIN, HIGH);
  digitalWrite(RED_LIGHTING_PIN, HIGH);
  Serial.begin(9600);
  Serial.print(F("\n\nSweets Box by H3\nhttps://blog.hacker3000.cf/sweetsbox.html\n\n"));
  Serial.print(F("Serial Commands\n * 'C' -> Initialize Calibration and confirm its steps\n * 'T' -> Reset starting weight to current weight (requires button interaction)\n * 'L' -> Read all logged resets in EEPROM\n * 'U' -> Unlock lock_in\n * 'R' -> Reset all settings to factory\n * 'I' -> initialize logging EEPROM\n * 'D' -> Dump all EEPROMs\n\n"));

  lid_lock.attach(SERVO_PIN);
  setServo(SERVO_LID_CLOSE);

  Wire.begin();  //this is for the logging EEPROM

  //LCD Setup
  Serial.println(F("Setting up LCD and saying hello..."));
  lcd.begin(16, 2);  //Set up a 16x2 display because that is is what this project is designed for
  lcd.clear();
  lcd.home();
  lcd.print(F("Sweets Box by H3"));
  lcd.setCursor(0, 1);
  lcd.print(F("-> hacker3000.cf"));
  int16_t blip_delay = 1000;
  for (uint8_t blip = 0; blip < 4; blip++) {  //commodore PET like startup sound
    tone(BUZZER_PIN, 1000);
    delay(25);
    tone(BUZZER_PIN, 2000);
    delay(25);
    noTone(BUZZER_PIN);
    delay(50);

    blip_delay -= 100;
  }
  delay(blip_delay);

  //rtc setup
  Serial.println(F("Setting up RTC..."));
  lcd.clear();
  lcd.home();
  lcd.print(F("RTC Setup..."));
  lcd.setCursor(0, 1);
  if (RTC.read(current_tm)) {  //if valid date read
    lcd.print(F("OK"));
  } else {
    if (RTC.chipPresent()) {  //if date invalid but RTC present
      Serial.println(F("RTC NOT SET"));
      lcd.setCursor(0, 0);
      lcd.print(F("RTC NOT SET"));
      lcd.setCursor(0, 1);
      lcd.print(F("Setings->SetDate"));  //Typo on purpose, 16char limit
      delay(5000);
    } else {  //if RTC missing
      Serial.println(F("RTC MISSING!"));
      lcd.print(F("RTC MISSING"));
      while (true) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
      }
    }
  }

  //load config
  Serial.println(F("Loading config from EEPROM..."));
  lcd.clear();
  lcd.home();
  lcd.print(F("Load EEPROM..."));
  bool needs_new_starting_weight = true;
  long scale_offset = 0;
  float scale_scale = 1157;  //default to this for 1kg load cells
  //Load config from EEPROM if magic number is there
  if (EEPROM.read(EEPROM.length() - 1 /*magic numer address for cal*/) == EEPROM_MAGIC_NUMBER) {  //read cal
    //get config from EEPROM
    EEPROM.get(getVarAddrEEPROM(SCALE_OFFSET_EEPROM_ADDR), scale_offset);
    EEPROM.get(getVarAddrEEPROM(SCALE_SCALE_EEPROM_ADDR), scale_scale);

    Serial.print(F("Scale Offset: "));
    Serial.println(scale_offset);
    Serial.print(F("Scale Scale: "));
    Serial.println(scale_scale);
  } else {  //warn if no calibration in EEPROM
    Serial.println(F("No valid calibration in EEPROM!\nPlease perform one by sending the character 'C' via Serial."));
    fullDispMsg(F("No Calibration"), F("in EEPROM!"));

    tone(BUZZER_PIN, 1000);
    delay(100);
    tone(BUZZER_PIN, 500);
    delay(100);
    noTone(BUZZER_PIN);

    delay(3000);

    fullDispMsg(F("Please go to"), F("Settings, and"));
    delay(2000);

    fullDispMsg(F("perform"), F("a Calibration"));
    delay(2000);
  }


  if (EEPROM.read(EEPROM.length() - 2 /*magic number address for config*/) == EEPROM_MAGIC_NUMBER) {  //read config
    EEPROM.get(getVarAddrEEPROM(MAX_WITHDRAW_EEPROM_ADDR), max_withdraw_per_day);
    EEPROM.get(getVarAddrEEPROM(BUZZER_ENABLED_EEPROM_ADDR), buzzer_enabled);


    Serial.print(F("Maximum per Day: "));
    Serial.println(max_withdraw_per_day);
    Serial.print(F("Buzzer enabled: "));
    Serial.println(buzzer_enabled);

    //if it was rebooted on the same day, keep the starting weight
    uint8_t day_starting_weight_day = 255;
    EEPROM.get(getVarAddrEEPROM(DAY_START_WEIGHT_DAY_EEPROM_ADDR), day_starting_weight_day);
    Serial.print(F("Last starting weigt from: "));
    Serial.println(day_starting_weight_day);
    if (day_starting_weight_day == current_tm.Day) {
      last_day = day_starting_weight_day;

      EEPROM.get(getVarAddrEEPROM(DAY_START_WEIGHT_EEPROM_ADDR), day_start_weight);
      Serial.print(F("Recoverd Day Starting Weight: "));
      Serial.println(day_start_weight);

      EEPROM.get(getVarAddrEEPROM(LOCK_IN_EEPROM_ADDR), lock_in);
      Serial.print(F("Recoverd LockIn: "));
      Serial.println(lock_in);
    }
  } else {  //warn if no config on EEPROM
    Serial.println(F("NO CONFIG IN EEPROM!"));
    fullDispMsg(F("No Config"), F("in EEPROM!"));

    tone(BUZZER_PIN, 1000);
    delay(100);
    tone(BUZZER_PIN, 500);
    delay(100);
    noTone(BUZZER_PIN);


    delay(4000);

    setting_reset();
    EEPROM.write(EEPROM.length() - 2, EEPROM_MAGIC_NUMBER);  //set magic number

    fullDispMsg(F("Please go to"), F("Settings and"));
    delay(2000);

    fullDispMsg(F("set all Settings"), F("to desired val"));
    delay(2000);
  }

  //Set up Load Cell Amp
  Serial.println(F("Setting up Load Cell Amp"));
  lcd.clear();
  lcd.home();
  lcd.print(F("Scale Setup..."));
  scale.begin(SCALE_DATA_PIN, SCALE_CLOCK_PIN);
  if (!scale.wait_ready_timeout(3000, 10)) {
    lcd.setCursor(0, 1);
    lcd.print(F("ERROR"));
    Serial.println(F("ERROR: Cant connect to load cell!"));
    while (true) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(100);
    }
  }

  //set offsets from EEPROM
  scale.set_offset(scale_offset);
  scale.set_scale(scale_scale);
  scale.set_medavg_mode();

  waitTilStable();

  //Set up button stuff last to avoid interrupts during startup
  Serial.println(F("Setting up button interrupts..."));
  pinMode(BTN_1_PIN, INPUT_PULLUP);
  pinMode(BTN_2_PIN, INPUT_PULLUP);
  pinMode(LID_SENSOR_PIN, LID_SENSOR_NO_INVERT ? INPUT : INPUT_PULLUP);  //Technically not a button. whatever

  attachInterrupt(digitalPinToInterrupt(BTN_1_PIN), handleButton1Down, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_2_PIN), handleButton2Down, FALLING);

  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(GREEN_LIGHTING_PIN, LOW);
  digitalWrite(RED_LIGHTING_PIN, LOW);
}

void loop() {  //This loop is very simple
  auto loop_start_millis = millis();

  handleButtons();
  readDevices();
  manageLimiting();
  updateDisplay();
  handleSerialControl();

  /*Serial.print(F("Loop time: "));
    Serial.print(millis() - loop_start_millis);
    Serial.println(F("ms"));*/
}
