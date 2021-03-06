/**
  ******************************************************************************
  * File Name          : main.c
  * Description        : Main program body
  ******************************************************************************
  *
  * COPYRIGHT(c) 2016 STMicroelectronics
  *
  * Redistribution and use in source and binary forms, with or without modification,
  * are permitted provided that the following conditions are met:
  *   1. Redistributions of source code must retain the above copyright notice,
  *      this list of conditions and the following disclaimer.
  *   2. Redistributions in binary form must reproduce the above copyright notice,
  *      this list of conditions and the following disclaimer in the documentation
  *      and/or other materials provided with the distribution.
  *   3. Neither the name of STMicroelectronics nor the names of its contributors
  *      may be used to endorse or promote products derived from this software
  *      without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */
/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spi.h"
#include "i2c.h"
#include "dwOps.h"
#include "cfg.h"
#include "mac.h"

#include "eeprom.h"
#include "led.h"
#include "usart.h"
#include "gpio.h"
#include "dma.h"

#include "libdw1000_pro.h"

extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart1;
volatile uint16_t timesIntoUsart2DMA = 0; //进入usart2接收中断的次数
/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

static CfgMode mode = modeAnchor;

uint8_t address[] = {0x07,0,0,0,0,0,0xcf,0xbc};       //存储的是源Tag地址
uint8_t base_address[] = {0,0,0,0,0,0,0xcf,0xbc};  //存储的是目标Anchor地址
uint8_t coordinator_address[] = {0x05,0,0,0,0,0,0xcf,0xbc};  //存储的是协调器地址
uint8_t tmp_address[] = {0,0,0,0,0,0,0xcf,0xbc};  //中间有一步要存储源Tag的地址，存到这个数组里面备份

#define debug(...) //printf(__VA_ARGS__)

// Static system configuration
#define MAX_ANCHORS 6    //没有使用
uint8_t anchors[MAX_ANCHORS];

// The four packets for ranging
#define POLL 0x01   // Poll is initiated by the tag
#define ANSWER 0x02
#define FINAL 0x03
#define REPORT 0x04 // Report contains all measurement from the anchor
#define POLL_MSG_TO_ANCHOR 0x05  // 发送到anchor节点的生理参数消息
#define ANSWER_MSG_FROM_ANCHOR 0x06  // 发送生理参数消息到Anchor后，Anchor会返回此应答，表示接收到了消息
#define POLL_MSG_TO_COORDINATOR 0x07  // 发送到anchor节点的生理参数消息
#define ANSWER_MSG_FROM_COORDINATOR 0x08  // 发送生理参数消息到协调器后，协调器会返回此应答，表示接收到了消息

// 2 Msg Types
#define MSG_TYPE_DISTANCE 0x01    //发送距离的包类型
#define MSG_TYPE_VITALSIGNS 0x02  //发送生理参数的包类型

//Justin add -- start
//#pragma anon_unions
//Tag计算距离之后，发送tag和测距基站的地址，还有距离信息，规定只是发送给地址1的基站
typedef struct {
	uint8_t tagAddress[8];
  uint8_t anchorAddress[8];
  uint32_t distance;
} __attribute__ ((packed)) reportDistance_t;
//Justin add -- end

typedef struct {
  uint8_t pollRx[5];
  uint8_t answerTx[5];
  uint8_t finalRx[5];

  char *test[];
} __attribute__ ((packed)) reportPayload_t;

// 定义了时间戳共用体，目的是为了存储“发送时间戳”、“接收时间戳”等，但是没有用到，因为
// libdw1000Types.h里面已经定义了dwTime_t，和此处的定义一样
#pragma anon_unions
typedef union timestamp_u {
  uint8_t raw[5];
  uint64_t full;
  struct {
    uint32_t low32;
    uint8_t high8;
  } __attribute__ ((packed));
  struct {
    uint8_t low8;
    uint32_t high32;
  } __attribute__ ((packed));
}__attribute__ ((packed)) timestamp_t;

// Timestamps for ranging，结合《dw1000 User Manual》第231页的图理解为什么要这么多时间戳
dwTime_t poll_tx;
dwTime_t poll_rx;
dwTime_t answer_tx;
dwTime_t answer_rx;
dwTime_t final_tx;
dwTime_t final_rx;

uint32_t rangingTick; //不知道什么作用

float pressure, temperature, asl;
bool pressure_ok;

const double C = 299792458.0;       // Speed of light
const double tsfreq = 499.2e6 * 128;  // Timestamp counter frequency，《dw1000 User Manual V2.17》P73中7.2.8中讲解了

#define ANTENNA_OFFSET 154.6   // In meter No PA
//#define ANTENNA_OFFSET 155.15   // In meter With PA
#define ANTENNA_DELAY  (ANTENNA_OFFSET*499.2e6*128)/299792458.0 // In radio tick

packet_t rxPacket;  // 这个里面定义了MAC数据格式，包括：MAC Header(21 octets)+ payload(64 octets)
packet_t txPacket;
static volatile uint8_t curr_seq = 0;

// Sniffer queue
#define QUEUE_LEN 16
packet_t snifferPacketQueue[QUEUE_LEN];
int snifferPacketLength[QUEUE_LEN];
int queue_head = 0;
int queue_tail = 0;
volatile uint32_t dropped = 0;

//Justin Add
uint32_t timeRangingComplete = 0;
uint8_t rangingComplete = 0;
volatile uint8_t MSG_TO_ANCHOR_FINISH = 0;
//volatile bool rangingLock = false;//当处于测距状态的时候rangingLock设置为true
//uint32_t rangingLockTick = 0;//
//uint8_t rangingTagAddr = 0xFF;//正在测距的tag地址
#define rangingOverTime 10 //over time 10ms,normally measure ranging time about 5ms
#define msgOverTime 400

//#define OLEDDISPLAY
#define USE_FTDI_UART

#define APP_NAME "DWM1000DISCOVERY"
#define AUTHOR_NAME "    -- By Justin"
#define DISTANCE_NAME "Distance:"

/* String used to display measured distance on LCD screen (16 characters maximum). */
char dist_str[16] = {0};

//uart packet
int32_t distanceAnchorX[6] = {0};
uint8_t uartPacket[32] = {
0x55,0xAA,/*head*/
0x13,/*len*/
0x00,/*ver*/
0x21,/*cmd*/
0x01,0x78,0x11,0x00,0x00,/*With Anchor 1 distance*/
0x02,0x78,0x11,0x00,0x00,/*With Anchor 2 distance*/
0x03,0xD0,0x07,0x00,0x00,/*With Anchor 3 distance*/
0xFB,0xDB	/*CRC16*/
};

uint8_t uartPacket4Anchor[27] = {
0x55,0xAA,/*head*/
0x1B,/*len*/
0x00,/*ver*/
0x22,/*cmd*/
0x01,0x00,0x00,0x00,0x00,/*With Anchor 1 distance*/
0x02,0x00,0x00,0x00,0x00,/*With Anchor 2 distance*/
0x03,0x00,0x00,0x00,0x00,/*With Anchor 3 distance*/
0x04,0x00,0x00,0x00,0x00,/*With Anchor 4 distance*/
0xFB,0xDB	/*CRC16*/
};

//Justin Add end


/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);


/* USER CODE BEGIN 0 */

#define TYPE 0
#define TYPE_MSG 1
#define SEQ_RANGING_MSG 2
#define SEQ_USART2_MSG_HIGH 3
#define SEQ_USART2_MSG_LOW 4
#define MSG 5
uint64_t distCount = 0;

/********************************************************************************************************/
void txcallback(dwDevice_t *dev)
{
  dwTime_t departure;
  dwGetTransmitTimestamp(dev, &departure);

  debug("TXCallback: ");
  switch (txPacket.payload[0]) {
		case POLL_MSG_TO_ANCHOR:
			debug("POLL_MSG_TO_ANCHOR\r\n");
			break;
		case ANSWER_MSG_FROM_ANCHOR:
			debug("ANSWER_MSG_FROM_ANCHOR\r\n");
		  break;
		case POLL_MSG_TO_COORDINATOR:
			debug("POLL_MSG_TO_COORDINATOR\r\n");
			break;
		case ANSWER_MSG_FROM_COORDINATOR:
			debug("ANSWER_MSG_FROM_COORDINATOR\r\n");
		  break;
    case POLL:
      rangingTick = HAL_GetTick();  // 记下Tag发送消息时的系统时间，注意与dw1000的发送时间戳区别开来
      debug("POLL\r\n");
      poll_tx = departure;
      break;
    case ANSWER:
      debug("ANSWER to %02x at %04x\r\n", txPacket.destAddress[0], (unsigned int)departure.low32);  // 为什么只有low32，不是一共40bit吗，见register:0x17 Transmit Time Stamp
      answer_tx = departure;
      break;
    case FINAL:
      debug("FINAL\r\n");
      final_tx = departure;
      break;
    case REPORT:
      debug("REPORT\r\n");
      break;
  }
}

/************************************************************************************************************/
void rxcallback(dwDevice_t *dev) {
	uint16_t i = 0;
  dwTime_t arival;
  int dataLength = dwGetDataLength(dev);

  if (dataLength == 0) return;

	//TODO
	memset(&rxPacket,0,MAC802154_HEADER_LENGTH);  //为什么要设为0

  debug("RXCallback(%d): ", dataLength);

  if (mode == modeSniffer) {
    if ((queue_head+1)%QUEUE_LEN == queue_tail) {
      dropped++;
      dwNewReceive(dev);
      dwSetDefaults(dev);
      dwStartReceive(dev);
      return;
    }

    // Queue the received packet, the main loop will print it on the console
    dwGetData(dev, (uint8_t*)&snifferPacketQueue[queue_head], dataLength);
    snifferPacketLength[queue_head] = dataLength;
    queue_head = (queue_head+1)%QUEUE_LEN;
    dwNewReceive(dev);
    dwSetDefaults(dev);
    dwStartReceive(dev);
    return;
  }

  dwGetData(dev, (uint8_t*)&rxPacket, dataLength);
		
  if (memcmp(rxPacket.destAddress, address, 8)) {
    debug("Not for me! for %02x with %02x\r\n", rxPacket.destAddress[0], rxPacket.payload[0]);
    dwNewReceive(dev);
    dwSetDefaults(dev);
    dwStartReceive(dev);
    return;
  }
	
//	//如果处于测距状态，但是其他的tag同时也要测距，直接返回
//	if(rangingLock == true && !(rangingTagAddr == rxPacket.sourceAddress[0]))
//	{
//		debug("ranging with tag %02x,but tag %02x want to rang\r\n", rxPacket.destAddress[0], rangingTagAddr);
//    dwNewReceive(dev);
//    dwSetDefaults(dev);
//    dwStartReceive(dev);
//		return;
//	}

  //dwGetReceiveTimestamp(dev, &arival);
	//debug("ReceiveTimestamp\r\n");
	
  memcpy(txPacket.destAddress, rxPacket.sourceAddress, 8);
  memcpy(txPacket.sourceAddress, rxPacket.destAddress, 8);

  switch(rxPacket.payload[TYPE]) {
    // Anchor received messages
		case POLL_MSG_TO_ANCHOR:  // 表示接收到的数据包是生理参数包
			debug("POLL_MSG_TO_ANCHOR\r\n");
		  if(dataLength - 26 > 0)
			{				
				memcpy(txPacket.payload, rxPacket.payload, dataLength-21);  //复制消息
				txPacket.payload[TYPE] = POLL_MSG_TO_COORDINATOR;
				memcpy(tmp_address, rxPacket.sourceAddress, 8);  //把Tag源地址存起来
				memcpy(txPacket.destAddress, coordinator_address, 8);  //重置目标地址为Coordinator，把消息转发给Coordinator
				
				dwNewTransmit(dev);
				dwSetDefaults(dev);
				dwSetData(dev, (uint8_t*)&txPacket, dataLength);
				dwWaitForResponse(dev, true);
				dwStartTransmit(dev);
			}
			break;
		case ANSWER_MSG_FROM_COORDINATOR:
			debug("ANSWER_MSG_FROM_COORDINATOR\r\n");
			txPacket.payload[TYPE] = ANSWER_MSG_FROM_ANCHOR;
		  txPacket.payload[TYPE_MSG] = rxPacket.payload[TYPE_MSG];
			txPacket.payload[SEQ_RANGING_MSG] = rxPacket.payload[SEQ_RANGING_MSG];
			txPacket.payload[SEQ_USART2_MSG_HIGH] = rxPacket.payload[SEQ_USART2_MSG_HIGH];
			txPacket.payload[SEQ_USART2_MSG_LOW] = rxPacket.payload[SEQ_USART2_MSG_LOW];
		  memcpy(txPacket.destAddress, tmp_address, 8);  //重置地址，把消息转发给Tag
		
			dwNewTransmit(dev);
			dwSetDefaults(dev);
			dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+5);
			dwWaitForResponse(dev, true);
			dwStartTransmit(dev);	
			break;
    case POLL:  // 表示接收到的数据包是测距包
      debug("POLL from %02x at %04x\r\n", rxPacket.sourceAddress[0], (unsigned int)arival.low32);
		
//			rangingTagAddr = rxPacket.sourceAddress[0];//记录Anchor正在跟哪个tag进行测距
//			rangingLock = true;//Anchor处于测距状态，lock住
//			rangingLockTick = HAL_GetTick();
		
      rangingTick = HAL_GetTick();  // 记下Anchor接收消息时的系统时间，注意与dw1000的发送时间戳区别开来

      txPacket.payload[TYPE] = ANSWER;
		  txPacket.payload[TYPE_MSG] = rxPacket.payload[TYPE_MSG];
      txPacket.payload[SEQ_RANGING_MSG] = rxPacket.payload[SEQ_RANGING_MSG];

      dwNewTransmit(dev);
      dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+3);
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
		
			dwGetReceiveTimestamp(dev, &arival);  //从DW1000的寄存器获取接收时间戳
      poll_rx = arival;
			break;
    // Coordinator received messages
		case POLL_MSG_TO_COORDINATOR:
			debug("POLL_MSG_TO_COORDINATOR\r\n");		
		  // 协调器接收到消息之后，把它通过串口发送给电脑
		  for(i = 0; i<dataLength-21; i++){
				  printf("%d ", rxPacket.payload[i]);
			}
			//HAL_UART_Transmit_DMA(&huart1, rxPacket.payload, dataLength-21);
			printf("\r\n");
			memset(&rxPacket.payload[MSG], 0, USART2_REC_LEN_DMA);
			
			// 给Anchor发送一个应答信号，表示接收到了消息
			txPacket.payload[TYPE] = ANSWER_MSG_FROM_COORDINATOR;
			txPacket.payload[TYPE_MSG] = rxPacket.payload[TYPE_MSG];
			txPacket.payload[SEQ_RANGING_MSG] = rxPacket.payload[SEQ_RANGING_MSG];
			txPacket.payload[SEQ_USART2_MSG_HIGH] = rxPacket.payload[SEQ_USART2_MSG_HIGH];
			txPacket.payload[SEQ_USART2_MSG_LOW] = rxPacket.payload[SEQ_USART2_MSG_LOW];	

			dwNewTransmit(dev);
			dwSetDefaults(dev);
			dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+5);
			dwWaitForResponse(dev, true);
			dwStartTransmit(dev);			
			break;
		// Tag received messages
		case ANSWER_MSG_FROM_ANCHOR:  // 表示接收到的数据包是生理参数应答包
			debug("ANSWER_MSG_FROM_ANCHOR\r\n");	
      if(rxPacket.payload[TYPE_MSG] == MSG_TYPE_VITALSIGNS){		
		    USART2_RX_STA = 0;
			}else if(rxPacket.payload[TYPE_MSG] == MSG_TYPE_DISTANCE){
				MSG_TO_ANCHOR_FINISH = 1;
			}else{
			}
			break;
    case ANSWER:
      debug("ANSWER\r\n");

      if (rxPacket.payload[SEQ_RANGING_MSG] != curr_seq) {
        debug("Wrong sequence number!SEQ_RANGING_MSG=%d, curr_seq=%d\r\n",rxPacket.payload[SEQ_RANGING_MSG],curr_seq);
        return;
      }

      txPacket.payload[0] = FINAL;
			txPacket.payload[TYPE_MSG] = rxPacket.payload[TYPE_MSG];
      txPacket.payload[SEQ_RANGING_MSG] = rxPacket.payload[SEQ_RANGING_MSG];

      dwNewTransmit(dev);
			dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+3);
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
			
			dwGetReceiveTimestamp(dev, &arival);
			answer_rx = arival;
      
			break;
    case FINAL:
    {
      reportPayload_t *report = (reportPayload_t *)(txPacket.payload+3);

      debug("FINAL\r\n");
			
			dwGetReceiveTimestamp(dev, &arival);
      final_rx = arival;		

      txPacket.payload[TYPE] = REPORT;
			txPacket.payload[TYPE_MSG] = rxPacket.payload[TYPE_MSG];
      txPacket.payload[SEQ_RANGING_MSG] = rxPacket.payload[SEQ_RANGING_MSG];
      memcpy(&report->pollRx, &poll_rx, 5);
      memcpy(&report->answerTx, &answer_tx, 5);
      memcpy(&report->finalRx, &final_rx, 5);

      dwNewTransmit(dev);
      dwSetDefaults(dev);
      dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+3+sizeof(reportPayload_t));
      dwWaitForResponse(dev, true);
      dwStartTransmit(dev);
			
//			//测距完成
//			rangingTagAddr = 0xFF;//记录Anchor正在跟哪个tag进行测距
//			rangingLock = false;//Anchor处于测距状态，lock住
//			
      break;
    }
    case REPORT:
    {
			//Justin add -- start
			/*
			if (mode == modeAnchor) {
				reportDistance_t *collectDistance = (reportDistance_t *)(rxPacket.payload+2);
				uint8_t anchorAddress, tagAddress;
				uint32_t aDistance;
				
				anchorAddress = collectDistance->anchorAddress[0];
				tagAddress = collectDistance->tagAddress[0];
				aDistance = collectDistance->distance;
				
				printf("tagAddress: %d,anchorAddress: %d, distance : %5d mm\r\n", tagAddress, anchorAddress, aDistance);
				return;
			}*/
			//Justin add -- end
						
      reportPayload_t *report = (reportPayload_t *)(rxPacket.payload+3);
      double tround1, treply1, treply2, tround2, tprop_ctn, tprop, distance;
			
      debug("REPORT\r\n");

      if (rxPacket.payload[SEQ_RANGING_MSG] != curr_seq) {
        debug("Wrong sequence number!SEQ_RANGING_MSG=%d, curr_seq=%d\r\n",rxPacket.payload[SEQ_RANGING_MSG],curr_seq);
        return;
      }

      memcpy(&poll_rx, &report->pollRx, 5);
      memcpy(&answer_tx, &report->answerTx, 5);
      memcpy(&final_rx, &report->finalRx, 5);

      debug("%02x%08x ", (unsigned int)poll_tx.high8, (unsigned int)poll_tx.low32);
      debug("%02x%08x\r\n", (unsigned int)poll_rx.high8, (unsigned int)poll_rx.low32);
      debug("%02x%08x ", (unsigned int)answer_tx.high8, (unsigned int)answer_tx.low32);
      debug("%02x%08x\r\n", (unsigned int)answer_rx.high8, (unsigned int)answer_rx.low32);
      debug("%02x%08x ", (unsigned int)final_tx.high8, (unsigned int)final_tx.low32);
      debug("%02x%08x\r\n", (unsigned int)final_rx.high8, (unsigned int)final_rx.low32);

      tround1 = answer_rx.low32 - poll_tx.low32;
      treply1 = answer_tx.low32 - poll_rx.low32;
      tround2 = final_rx.low32 - answer_tx.low32;
      treply2 = final_tx.low32 - answer_rx.low32;

      debug("%08x %08x\r\n", (unsigned int)tround1, (unsigned int)treply2);
      debug("\\    /   /     \\\r\n");
      debug("%08x %08x\r\n", (unsigned int)treply1, (unsigned int)tround2);

      tprop_ctn = ((tround1*tround2) - (treply1*treply2)) / (tround1 + tround2 + treply1 + treply2);

      debug("TProp (ctn): %d\r\n", (unsigned int)tprop_ctn);

      tprop = tprop_ctn/tsfreq;
			debug("tprop : %d\r\n", (unsigned int)tprop);
      distance = C * tprop;
			
			distCount++;
			
			//濡傛灉灏忎簬9绫筹紝绾犳璇樊10鍘樼背
			if(distance < 7.0)
			{
				distance +=0.15;
			}else if(distance >= 7.0 && distance <= 10.0)
			{
				distance += -0.05 * distance + 0.5;
			}
			
			distanceAnchorX[rxPacket.sourceAddress[0]] = distance*1000;

			switch (rxPacket.sourceAddress[0])
			{
				case 1:
					uartPacket4Anchor[6] = distanceAnchorX[1]&0xff;
					uartPacket4Anchor[7] = (distanceAnchorX[1]>>8)&0xff;
					uartPacket4Anchor[8] = (distanceAnchorX[1]>>16)&0xff;
					uartPacket4Anchor[9] = (distanceAnchorX[1]>>24)&0xff;
				  break;
				case 2:
					uartPacket4Anchor[11] = distanceAnchorX[2]&0xff;
					uartPacket4Anchor[12] = (distanceAnchorX[2]>>8)&0xff;
					uartPacket4Anchor[13] = (distanceAnchorX[2]>>16)&0xff;
					uartPacket4Anchor[14] = (distanceAnchorX[2]>>24)&0xff;
				  break;
				case 3:
					uartPacket4Anchor[16] = distanceAnchorX[3]&0xff;
					uartPacket4Anchor[17] = (distanceAnchorX[3]>>8)&0xff;
					uartPacket4Anchor[18] = (distanceAnchorX[3]>>16)&0xff;
					uartPacket4Anchor[19] = (distanceAnchorX[3]>>24)&0xff;
				  break;
				case 4:
					uartPacket4Anchor[21] = distanceAnchorX[4]&0xff;
					uartPacket4Anchor[22] = (distanceAnchorX[4]>>8)&0xff;
					uartPacket4Anchor[23] = (distanceAnchorX[4]>>16)&0xff;
					uartPacket4Anchor[24] = (distanceAnchorX[4]>>24)&0xff;
					break;
				default:
					break;
			}
				
				//HAL_UART_Transmit(&huart1, uartPacket4Anchor, 27, HAL_MAX_DELAY);
			
			printf("distance%d: %6dmm\r\n", rxPacket.sourceAddress[0], (unsigned int)(distance*1000));
			
			#ifdef OLEDDISPLAY
				//OLED_ShowString(0,6,"                 ");//clear the line
				sprintf(dist_str, "DIST: %6dmm", (unsigned int)(distance*1000));

				OLED_ShowString(0,6,(uint8_t*)dist_str);
			#endif
			if(rxPacket.sourceAddress[0] == 4)
			{
				printf("\r\n");
			}

			dwGetReceiveTimestamp(dev, &arival);
      debug("Total in-air time (ctn): 0x%08x\r\n", (unsigned int)(arival.low32-poll_tx.low32));
			debug("Total in-air time (ctn): %.4f s\r\n", (float)((arival.low32-poll_tx.low32)/tsfreq));
			
			//Justin add -- start
			//娴嬭窛瀹屾垚鍚庯紝灏唕angingComplete璁剧疆涓簍rue锛岃〃绀轰竴娆℃祴璺濆畬鎴�
			rangingComplete = 1;
			//Justin add -- end

      break;
    }
  }
}

/*********************************************************************************************************/
/* USER CODE END 0 */
dwDevice_t dwm_device;
dwDevice_t *dwm = &dwm_device;

int main(void)
{
	
	int result=0;
  int i=0;
  bool selftestPasses = true;
	
	uint8_t anchorListSize = 0; //eeprom中存储的Anchor的数目

  bool ledState = false;
  uint32_t ledTick = 0;
	
	char msg[90];  //只是为了输出一些调试信息，在正式版本里可以去掉
	uint8_t Usart2_Start_CMD[4] = {0xCC, 0xCC, 0x00, 0x00};
	
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration----------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Configure the system clock */
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
	MX_DMA_Init();
	MX_USART1_UART_Init();
	MX_USART2_UART_Init();
  MX_SPI1_Init();
	MX_I2C1_Init();
	LED_init();
	//MYDMA_Config();
  // Light up all LEDs to test
  ledOn(ledRanging);
  ledOn(ledSync);
  ledOn(ledMode);
	ledOn(ledUser1);
	ledOn(ledUser2);
	
	printf("TEST\t: EEPROM self-test\r\n");
	eepromInit(&hi2c1);
	
  cfgInit(); // 配置eeprom

  if (cfgReadU8(cfgAddress, &address[0])) {
    printf("CONFIG\t: Address is 0x%X\r\n", address[0]);  //这儿就把DWM1000的地址从EEPROM中读出来，存到了address[0]中
  } else {
    printf("CONFIG\t: Address not found!\r\n");
  }

  if (cfgReadU8(cfgMode, &mode)) {
    printf("CONFIG\t: Mode is ");
    switch (mode) {
      case modeAnchor: printf("Anchor\r\n"); break;
      case modeTag: printf("Tag\r\n"); break;
      case modeSniffer: printf("Sniffer\r\n"); break;
      default: printf("UNKNOWN\r\n"); break;
    }
  } else {
    printf("Device mode: Not found!\r\n");
  }


  if (cfgFieldSize(cfgAnchorlist, &anchorListSize)) {
    if (cfgReadU8list(cfgAnchorlist, (uint8_t*)&anchors, anchorListSize)) {
      printf("CONFIG\t: Tag mode anchor list (%i): ", anchorListSize);
      for (i = 0; i < anchorListSize; i++) {
        printf("0x%02X ", anchors[i]);
      }
      printf("\r\n");
    } else {
      printf("CONFIG\t: Tag mode anchor list: Not found!\r\n");
    }
  }
	
	printf("TEST\t: Initialize DWM1000 ... \r\n");
	dwInit(dwm, &dwOps);       // Init libdw
  dwOpsInit(dwm);            // Init interrupt
  result = dwConfigure(dwm); // Configure the dw1000 chip
	
  if (result == 0) {
    //dwAddPA(dwm);      //这是作者自己写的，为了使用外部功率放大器，没有使用的话，就屏蔽掉
		printf("[OK]\r\n");
    dwEnableAllLeds(dwm);
		LED_on(LEDY_RANGING);
  } else {
    printf("[ERROR]: %s\r\n", dwStrError(result));
    selftestPasses = false;
		LED_on(LEDG_SYNC);
  }
	
	HAL_Delay(100);

	// switch off all LEDs
  ledOff(ledRanging);
  ledOff(ledSync);
  ledOff(ledMode);
	ledOff(ledUser1);
	ledOff(ledUser2);
	
	dwTime_t delay = {.full = ANTENNA_DELAY/2};//delay =16475.679
  dwSetAntenaDelay(dwm, delay);

  dwAttachSentHandler(dwm, txcallback);
  dwAttachReceivedHandler(dwm, rxcallback);

  dwNewConfiguration(dwm);                                 // new一个新配置
  dwSetDefaults(dwm);
	//dwEnableMode(dwm, MODE_LONGDATA_RANGE_LOWPOWER);	//MODE_LONGDATA_RANGE_LOWPOWER[] = {TRX_RATE_110KBPS, TX_PULSE_FREQ_16MHZ, TX_PREAMBLE_LEN_2048}
	//dwEnableMode(dwm, MODE_SHORTDATA_FAST_LOWPOWER);	//MODE_SHORTDATA_FAST_LOWPOWER[] = {TRX_RATE_6800KBPS, TX_PULSE_FREQ_16MHZ, TX_PREAMBLE_LEN_128}
	//dwEnableMode(dwm, MODE_LONGDATA_FAST_LOWPOWER);		//MODE_LONGDATA_FAST_LOWPOWER[] = {TRX_RATE_6800KBPS, TX_PULSE_FREQ_16MHZ, TX_PREAMBLE_LEN_1024}
	//dwEnableMode(dwm, MODE_SHORTDATA_FAST_ACCURACY);	//MODE_SHORTDATA_FAST_ACCURACY[] = {TRX_RATE_6800KBPS, TX_PULSE_FREQ_64MHZ, TX_PREAMBLE_LEN_128}
	dwEnableMode(dwm, MODE_LONGDATA_FAST_ACCURACY);     //MODE_LONGDATA_FAST_ACCURACY[] = {TRX_RATE_6800KBPS, TX_PULSE_FREQ_64MHZ, TX_PREAMBLE_LEN_1024}
	//dwEnableMode(dwm, MODE_LONGDATA_RANGE_ACCURACY);	//MODE_LONGDATA_RANGE_ACCURACY[] = {TRX_RATE_110KBPS, TX_PULSE_FREQ_64MHZ, TX_PREAMBLE_LEN_2048}
  dwSetChannel(dwm, CHANNEL_2);
  //dwSetPreambleCode(dwm, PREAMBLE_CODE_64MHZ_9);
	dwSetPreambleCode(dwm, PREAMBLE_CODE_16MHZ_3);
  dwCommitConfiguration(dwm);                              // 配置更新完后，提交新配置

  printf("SYSTEM\t: Node started ...\r\n");
  printf("SYSTEM\t: Press 'h' for help.\r\n");
	
	getPrintableDeviceIdentifier(dwm,msg);  //获取设备ID,应该是0xDECA0130
	printf("%s\r\n",msg);
	getPrintableExtendedUniqueIdentifier(dwm,msg);  //获取设备EUI
	printf("%s\r\n",msg); 
	getPrintableDeviceMode(dwm,msg);  //获取设备模式，包括Pulse Frequence,Preamble length,TXRX Rate, Channel and Preamble code
	printf("%s\r\n",msg);
	getPrintableNetworkIdAndShortAddress(dwm,msg);  //获取设备PANID和短地址
	printf("%s\r\n",msg);
	
  // Initialize the packet in the TX buffer
  MAC80215_PACKET_INIT(txPacket, MAC802154_TYPE_DATA);
  txPacket.destPANId = 0xbccf;  //这个pan是不是一个局域网地址？当使用Receive Frame Filtering时，应该把它写入0x03

  if (mode == modeAnchor || mode == modeSniffer) {
    dwNewReceive(dwm);
    dwSetDefaults(dwm);
    dwStartReceive(dwm);
  }
	
	HAL_UART_Receive_DMA(&huart1, (uint8_t *)USART1_RX_BUF, USART1_REC_LEN_DMA);
	HAL_UART_Receive_DMA(&huart2, (uint8_t *)USART2_RX_BUF, USART2_REC_LEN_DMA);
	HAL_Delay(10);
	HAL_UART_Transmit(&huart2, Usart2_Start_CMD, 4, HAL_MAX_DELAY);
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    // Accepts serial commands
		if(USART1_RX_STA){
      handleInput(USART1_RX_BUF[0]);
      USART1_RX_STA = 0;			
    }

    if (mode == modeSniffer) {
      static uint32_t prevDropped = 0;

      if (dropped != prevDropped) {
        printf("Dropped!\r\n");
        prevDropped = dropped;
      }

      if (queue_tail != queue_head) {
        printf("From %02x to %02x data ", snifferPacketQueue[queue_tail].sourceAddress[0],
                                          snifferPacketQueue[queue_tail].destAddress[0]);
        for (int i=0; i<(snifferPacketLength[queue_tail] - MAC802154_HEADER_LENGTH); i++) {
          printf("0x%02x ", snifferPacketQueue[queue_tail].payload[i]);
        }
        queue_tail = (queue_tail+1)%QUEUE_LEN;
        printf("\r\n");
      }
    }

    if (mode == modeTag) {
      if(USART2_RX_STA){  //接收到了USART2传来的生理参数信息，就把它发送出去
				base_address[0] = anchors[0];  //把消息发送给Anchor0，Anchor0充当路由节点功能
				dwIdle(dwm);
				
				txPacket.payload[TYPE] = POLL_MSG_TO_ANCHOR;  // 表示发送数据包是生理参数包,
				txPacket.payload[TYPE_MSG] = MSG_TYPE_VITALSIGNS;  //发送的数据包类型：生理参数信息包
				txPacket.payload[SEQ_RANGING_MSG] = curr_seq;  // 测距包的序号仍然和上一次测距一样，不改变
				txPacket.payload[SEQ_USART2_MSG_HIGH] = timesIntoUsart2DMA>>8;    // 生理参数数据包的序号 高8位
				txPacket.payload[SEQ_USART2_MSG_LOW] = timesIntoUsart2DMA;        // 低8位
				memcpy(&txPacket.payload[MSG], USART2_RX_BUF, USART2_REC_LEN_DMA);  // 把接收的生理参数信息复制到发送包中
				
				memcpy(txPacket.sourceAddress, address, 8);  // uint8_t address[8] 存储的是源地址
				memcpy(txPacket.destAddress, base_address, 8);   // base_address[0]存储的是目标地址
				
				dwNewTransmit(dwm);
        dwSetDefaults(dwm);
        dwSetData(dwm, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+5+USART2_REC_LEN_DMA);  //+5是因为有payload的5个字节，payload[TYPE], payload[TYPE_MSG], payload[SEQ_RANGING_MSG],
                                                                                              // payload[SEQ_USART2_MSG_HIGH],payload[SEQ_USART2_MSG_LOW],
				dwWaitForResponse(dwm, true);
				dwStartTransmit(dwm); 

        timeRangingComplete = HAL_GetTick();
				while(USART2_RX_STA == 1 && (HAL_GetTick()<(timeRangingComplete+msgOverTime)));

				if(HAL_GetTick()<(timeRangingComplete+msgOverTime)){
					printf("MSG Transmited Successfully! tag and Coordinator transmiting time: %04d ms\r\n",(unsigned int)(HAL_GetTick() - timeRangingComplete));
				}else{
					printf("MSG Transmited fail! tag and Coordinator transmiting time: %04d ms\r\n", (unsigned int)(HAL_GetTick() - timeRangingComplete));
					USART2_RX_STA = 0;
				}
			}else{	//没有接收到生理参数信息，就测距				
				for (i=0; i<4; i++) {					
					 //printf ("Interrogating anchor %d\r\n", anchors[i]);
					 base_address[0] = anchors[i];
					 dwIdle(dwm);

					 txPacket.payload[TYPE] = POLL;
					 txPacket.payload[TYPE_MSG] = MSG_TYPE_DISTANCE;  //发送的数据包类型：测距包
					 txPacket.payload[SEQ_RANGING_MSG] = ++curr_seq;
					 
					 memcpy(txPacket.sourceAddress, address, 8);      // uint8_t address[8] 存储的是源地址
					 memcpy(txPacket.destAddress, base_address, 8);   // base_address[0]存储的是目标地址

					 dwNewTransmit(dwm);
					 dwSetDefaults(dwm);
					 dwSetData(dwm, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+3); 
					 dwWaitForResponse(dwm, true);
					 dwStartTransmit(dwm);

					 //Justin add
					 //measure the ranging time,then printf the tag to every anchor ranging time information
					 timeRangingComplete = HAL_GetTick();
					 rangingComplete = 0;
					 while((rangingComplete ==0)&&(HAL_GetTick()<(timeRangingComplete+rangingOverTime)));    // 一直循环，直到完成测距，rangingComplete会被置1
					                                                                                         // 在这个循环过程中，会不断地进入接收中断，直到最后 report 时，rangingComplete = 1;
					 if(HAL_GetTick()<(timeRangingComplete+rangingOverTime)){
						 printf("Ranging Successfully! tag and anchor[%d] ranging time: %04d ms\r\n", (i+1),(unsigned int)(HAL_GetTick() - timeRangingComplete));
					 }else{
						 printf("Ranging fail! anchor[%d] maybe no exist, ranging time over: %04d ms\r\n", (i+1),(unsigned int)(HAL_GetTick() - timeRangingComplete));
					 }
					 //Justin add end
				 }
        //整个测完一次距离，（Tag到4个Anchor的距离），把距离发送给Anchor0
					base_address[0] = anchors[0];
					dwIdle(dwm);
					
					txPacket.payload[TYPE] = POLL_MSG_TO_ANCHOR;
					txPacket.payload[TYPE_MSG] = MSG_TYPE_DISTANCE;  //发送的数据包类型：距离包
					txPacket.payload[SEQ_RANGING_MSG] = curr_seq;
			  	txPacket.payload[SEQ_USART2_MSG_HIGH] = timesIntoUsart2DMA>>8;    // 生理参数数据包的序号 高8位
				  txPacket.payload[SEQ_USART2_MSG_LOW] = timesIntoUsart2DMA;        // 低8位
					memcpy(txPacket.sourceAddress, address, 8);      // uint8_t address[8] 存储的是源地址
					memcpy(txPacket.destAddress, base_address, 8);  //把消息发送给路由器
					memcpy(&txPacket.payload[MSG], uartPacket4Anchor, 27);  //复制距离消息
					
				  dwNewTransmit(dwm);
				  dwSetDefaults(dwm);
				  dwSetData(dwm, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+5+27); 
				  dwWaitForResponse(dwm, true);
				  dwStartTransmit(dwm);
					
				  timeRangingComplete = HAL_GetTick();
					MSG_TO_ANCHOR_FINISH = 0;
					while((MSG_TO_ANCHOR_FINISH == 0) && (HAL_GetTick()<(timeRangingComplete+msgOverTime)));
					
					if(HAL_GetTick()<(timeRangingComplete+msgOverTime)){
						printf("MSG Transmited Successfully! tag and Coordinator transmiting time: %04d ms\r\n",(unsigned int)(HAL_GetTick() - timeRangingComplete));
					}else{
						printf("MSG Transmited fail! tag and Coordinator transmiting time: %04d ms\r\n", (unsigned int)(HAL_GetTick() - timeRangingComplete));
						MSG_TO_ANCHOR_FINISH = 1;
					}					
			 }
		 }
	 
//		 if(HAL_GetTick() > rangingLockTick + 10)
//		 {
//				rangingLock = false;
//				rangingTagAddr = 0xFF;
//		 }

    // Handling of the LEDs
    if (HAL_GetTick() > (ledTick+250)) {
      ledTick = HAL_GetTick();
      ledState = !ledState;
			ledToggle(ledSync);
    }

     if ((HAL_GetTick() < (rangingTick+50)) && ledState) {
       ledOn(ledRanging);  // 说明正在测距
     } else {
       ledOff(ledRanging);
     }

     //ledOff(ledSync);

     switch (mode) {
       case modeTag:
         ledOff(ledMode);
         break;
       case modeAnchor:
         ledOn(ledMode);
         break;
       case modeSniffer:
         if (ledState) {
           ledOn(ledMode);
         } else {
           ledOff(ledMode);
         }
         break;
       default:
         ledOn(ledMode);
         ledOn(ledSync);
         ledOn(ledRanging);
         break;
     }

  }
  /* USER CODE END 3 */

}

/** System Clock Configuration
*/
void SystemClock_Config(void)
{

  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;

  __PWR_CLK_ENABLE();

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);

  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq()/1000);

  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}


/**
  * @brief  This function is executed in case of error occurrence.
  * @param  file: The file name as string.
  * @param  line: The line in file as a number.
  * @retval None
  */
void _Error_Handler(char *file, int line)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  while(1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT

/**
   * @brief Reports the name of the source file and the source line number
   * where the assert_param error has occurred.
   * @param file: pointer to the source file name
   * @param line: assert_param error line source number
   * @retval None
   */
void assert_failed(uint8_t* file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
    ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */

}

#endif

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
