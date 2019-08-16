/*
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * LPS node firmware.
 *
 * Copyright 2016, Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stm32f4xx_hal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cfg.h"
#include "eeprom.h"

typedef struct {
  uint8_t *data;
} TlvArea;

// Temporary fix, just buffer the first 100 bytes
#define NUMBER_OF_BYTES_READ 25
#define MAGIC ((uint8_t) 0xBC)

#define SIZE_HEADER 5
#define SIZE_TAIL 1

static uint8_t buffer[NUMBER_OF_BYTES_READ];

static TlvArea tlv;

typedef struct {
  uint8_t magic;
  uint8_t majorVersion;
  uint8_t minorVersion;
  uint16_t tlvSize;
} __attribute__((packed)) CfgHeader;

static CfgHeader * cfgHeader;

/********************************************************************************
//
*********************************************************************************/
static int tlvFindType(TlvArea *tlv, ConfigField type) {
  uint16_t pos = 0;

  while (pos < cfgHeader->tlvSize) {
    if (tlv->data[pos] == type) {
      return pos;
    } else {
      pos += tlv->data[pos+1]+2;  // 这一句什么意思？
    }
  }
  return -1;
}

/********************************************************************************
//
*********************************************************************************/
bool deckTlvHasElement(TlvArea *tlv, ConfigField type) {
  return tlvFindType(tlv, type) >= 0;
}

/********************************************************************************
//Reads all the data from the EEPROM
*********************************************************************************/
static bool readData(void) {
  int i;
  if (eepromRead(0, buffer, NUMBER_OF_BYTES_READ)) {
    printf("EEPROM: ");
    for (i = 0; i <NUMBER_OF_BYTES_READ; i++)
      printf("0x%02X ", buffer[i]);
    printf("\r\n");
    return true;
  } else {
    printf("CONFIG\t: Failed to read data from EEPROM!\r\n");
    return false;
  }
}

/********************************************************************************
//把buffer里面的数据全部加起来放在最后一位，为什么这样做
*********************************************************************************/
static void write_crc(void) {
  int i;
  uint8_t checksum = 0;

  for (i = 0; i < SIZE_HEADER + cfgHeader->tlvSize ; i++) {
    checksum += buffer[i];
  }
  buffer[SIZE_HEADER + cfgHeader->tlvSize] = checksum;
}

/********************************************************************************
//检查“buffer的最后一位”和“前面所有数据之和”是否相等
*********************************************************************************/
static bool check_crc(void) {
  int total_len = SIZE_HEADER + SIZE_TAIL + cfgHeader->tlvSize;
  uint8_t ref_checksum = buffer[total_len - SIZE_TAIL];
  int i;
  uint8_t checksum = 0;

  for (i = 0; i < SIZE_HEADER + cfgHeader->tlvSize; i++) {
    checksum += buffer[i];
  }
  if (checksum == ref_checksum) {
    return true;
  } else {
    printf("CONFIG\t: EEPROM configuration checksum not correct (0x%02X vs 0x%02X)!\r\n", ref_checksum, checksum);
    return false;
  }
}

/********************************************************************************
//
*********************************************************************************/
static bool check_magic(void) {
  return (cfgHeader->magic == MAGIC);
}

/********************************************************************************
//
*********************************************************************************/
static bool check_content(void) {
  if (check_magic()) {
    return check_crc();
  }
  printf("CONFIG\t: EEPROM magic not found!\r\n");
  return false;
}

/********************************************************************************
//如果eeprom里面没有数据，会写入默认的数据
*********************************************************************************/
static bool write_defaults(void) {
  uint8_t default_anchor_list[] = {1, 2, 3, 4, 5, 6};

  buffer[0] = MAGIC;
  buffer[1] = 1; // Major version
  buffer[2] = 0; // Minor version
  buffer[3] = 0; // Length of TLV
  buffer[4] = 0; // Length of TLV
  buffer[5] = buffer[0] + buffer[1];
  // Write the default address
  cfgWriteU8(cfgAddress, 0);
  cfgWriteU8(cfgMode, modeAnchor);
  cfgWriteU8list(cfgAnchorlist, default_anchor_list, sizeof(default_anchor_list));
  write_crc();
  if (!eepromWrite(0, buffer, 7))
    return false;
  HAL_Delay(10);
  if (readData()) {
    if (check_content()) {
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

/********************************************************************************
//
*********************************************************************************/
void cfgInit(void) {
  if (readData()) {
    cfgHeader = (CfgHeader*) buffer;
    tlv.data = &buffer[SIZE_HEADER];
    if (check_content()) {
      printf("CONFIG\t: EEPROM configuration read and verified\r\n");
    } else {
      printf("CONFIG\t: Writing default EEPROM configuration\r\n");
      if (write_defaults()) {
      } else {
        printf("CONFIG\t: Error when writing default configuration\r\n");
      }
    }
  } else {
    printf("CONFIG\t: Could not read data from EEPROM\r\n");
  }
}

/********************************************************************************
//这个函数是把eeprom里面的数据清除了
*********************************************************************************/
bool cfgReset(void) {
  uint8_t data = 0;
  bool ret = eepromWrite(0, &data, 1);
  HAL_Delay(10);
  return ret;
}

/********************************************************************************
//
*********************************************************************************/
bool cfgReadU8(ConfigField field, uint8_t * value) {
  int pos = tlvFindType(&tlv, field);

  if (pos > -1) {
    *value = tlv.data[pos+2];
  }

  return (pos > -1);
}

/********************************************************************************
//
*********************************************************************************/
bool cfgWriteU8(ConfigField field, uint8_t value) {
    int pos = tlvFindType(&tlv, field);

    if (pos > -1) {
      tlv.data[pos+2] = value;
    } else {
      // Add new field at the end of the tlv
      tlv.data[cfgHeader->tlvSize] = field;
      tlv.data[cfgHeader->tlvSize+1] = 1;
      tlv.data[cfgHeader->tlvSize+2] = value;
      cfgHeader->tlvSize += 3;
    }

    write_crc();
    eepromWrite(0, buffer, NUMBER_OF_BYTES_READ);
    HAL_Delay(10);
    readData();
    return true;
}

/********************************************************************************
//
*********************************************************************************/
bool cfgReadU8list(ConfigField field, uint8_t list[], uint8_t length) {
  int pos = tlvFindType(&tlv, field);

  if (pos > -1) {
    memcpy(list, &tlv.data[pos+2], length);
  }

  return (pos > -1);
}

/********************************************************************************
//
*********************************************************************************/
bool cfgFieldSize(ConfigField field, uint8_t * size) {
  int pos = tlvFindType(&tlv, field);

  if (pos > -1) {
    *size = tlv.data[pos+1];
  }

  return (pos > -1);
}

/********************************************************************************
//
*********************************************************************************/
bool cfgWriteU8list(ConfigField field, uint8_t list[], uint8_t length) {
    int pos = tlvFindType(&tlv, field);

    if (pos > -1) {
      printf("Witing the list is not supported!!\r\n");
      //tlv.data[pos+2] = value;
      // TODO: The list can vary in length, we need to take care of that :-(
    } else {
      // Add new field at the end of the tlv
      tlv.data[cfgHeader->tlvSize] = field;
      tlv.data[cfgHeader->tlvSize+1] = length;
      memcpy(&tlv.data[cfgHeader->tlvSize+2], list, length);
      cfgHeader->tlvSize += 2 + length;
    }

    write_crc();
    eepromWrite(0, buffer, NUMBER_OF_BYTES_READ);
    HAL_Delay(10);
    readData();
    return true;
}



// 以下函数是从main.c中搬移过来的，主要是一些 串口输出配置信息 或者 修改配置 的函数

extern uint8_t address[8];  //main.c中定义的
/********************************************************************************
//处理串口的输入信息
*********************************************************************************/
void handleInput(char ch) {
  bool configChanged = true;

  switch (ch) {
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      changeAddress(ch - '0');
      break;
    case 'a': changeMode(modeAnchor); break;
    case 't': changeMode(modeTag); break;
    case 's': changeMode(modeSniffer); break;
    case 'd': resetConfig(); break;
    case 'h':
      help();
      configChanged = false;
      break;
    case '#':
      //productionTestsRun();
      printf("System halted, reset to continue\r\n");
      while(true){}
      break;
    default:
      configChanged = false;
      break;
  }

  if (configChanged) {
    printf("EEPROM configuration changed, restart for it to take effect!\r\n");
  }
}

/********************************************************************************
//重置设备的配置信息
*********************************************************************************/
void resetConfig(void) {
  printf("Resetting EEPROM configuration...");
  if (cfgReset()) {
    printf("OK\r\n");
  } else {
    printf("ERROR\r\n");
  }
}

/********************************************************************************
//改变设备的地址
*********************************************************************************/
void changeAddress(uint8_t addr) {
  printf("Updating address from 0x%02X to 0x%02X\r\n", address[0], addr);
  cfgWriteU8(cfgAddress, addr);
  if (cfgReadU8(cfgAddress, &address[0])) {
    printf("Device address: 0x%X\r\n", address[0]);
  } else {
    printf("Device address: Not found!\r\n");
  }
}

/********************************************************************************
//改变设备的模式
*********************************************************************************/
void changeMode(CfgMode newMode) {
    printf("Previous device mode: ");
    printMode();

    cfgWriteU8(cfgMode, newMode);

    printf("New device mode: ");
    printMode();
}

/********************************************************************************
//串口打印出当前设备的模式
*********************************************************************************/
void printMode(void) {
  CfgMode mode;

  if (cfgReadU8(cfgMode, &mode)) {
    switch (mode) {
      case modeAnchor: printf("Anchor"); break;
      case modeTag: printf("Tag"); break;
      case modeSniffer: printf("Sniffer"); break;
      default: printf("UNKNOWN"); break;
    }
  } else {
    printf("Not found!");
  }

  printf("\r\n");
}

/********************************************************************************
//串口输出“帮助”信息
*********************************************************************************/
void help(void) {
  printf("Help\r\n");
  printf("-------------------\r\n");
  printf("0-9 - set address\r\n");
  printf("a   - anchor mode\r\n");
  printf("t   - tag mode\r\n");
  printf("s   - sniffer mode\r\n");
  printf("d   - reset configuration\r\n");
  printf("h   - This help\r\n");
}

