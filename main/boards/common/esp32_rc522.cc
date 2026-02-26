/*
 * 立创开发板软硬件资料与相关扩展板软硬件资料官网全部开源
 * 开发板官网：www.lckfb.com
 * 技术支持常驻论坛，任何技术问题欢迎随时交流学习
 * 立创论坛：club.szlcsc.com
 * 关注bilibili账号：【立创开发板】，掌握我们的最新动态！
 * 不靠卖板赚钱，以培养中国工程师为己任
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-10     LCKFB-lp    first version
 */
#include "esp32_rc522.h"



void delay_ms(unsigned int ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}
void delay_us(unsigned int us)
{
    esp_rom_delay_us(us);
}
void delay_1ms(unsigned int ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}
void delay_1us(unsigned int us)
{
    ets_delay_us(us);
}
/******************************************************************
 * 函 数 名 称：RC522_Init
 * 函 数 说 明：IC卡感应模块配置
 * 函 数 形 参：无
 * 函 数 返 回：无
 * 作       者：LC
 * 备       注：
******************************************************************/
void RC522_Init(void)
{
    gpio_config_t out_config = {
        .pin_bit_mask = (1ULL<<GPIO_CS)|(1ULL<<GPIO_SCK)|(1ULL<<GPIO_MOSI)|(1ULL<<GPIO_RST),    //配置引脚
        .mode =GPIO_MODE_OUTPUT,                  //输出模式
        .pull_up_en = GPIO_PULLUP_ENABLE,         //使能上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,    //不使能下拉
        .intr_type = GPIO_INTR_DISABLE            //不使能引脚中断
        };
    gpio_config(&out_config);

    gpio_config_t in_config = {
        .pin_bit_mask = (1ULL<<GPIO_MISO),         //配置引脚
        .mode =GPIO_MODE_INPUT,                    //输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,         //不使能上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,     //不使能下拉
        .intr_type = GPIO_INTR_DISABLE             //不使能引脚中断
        };
    gpio_config(&in_config);

}

////////////////软件模拟SPI与RC522通信///////////////////////////////////////////
/* 软件模拟SPI发送一个字节数据，高位先行 */
void RC522_SPI_SendByte( uint8_t byte )
{
        uint8_t n;
        for( n=0;n<8;n++ )
        {
                if( byte&0x80 )
                        RC522_MOSI_1();
                else
                        RC522_MOSI_0();

                delay_us(1);
                RC522_SCK_0();
                delay_us(1);
                RC522_SCK_1();
                delay_us(1);

                byte<<=1;
        }
}

/* 软件模拟SPI读取一个字节数据，先读高位 */
uint8_t RC522_SPI_ReadByte( void )
{
    uint8_t n;
    uint8_t data = 0;
    for( n=0;n<8;n++ )
    {
        data <<= 1;

        RC522_SCK_0();
        delay_us(1);

        if( RC522_MISO_GET()==1 )
        {
            data|=0x01;
        }
        delay_us(1);
        RC522_SCK_1();
        delay_us(1);


    }
    return data;
}

//////////////////////////GD32对RC522寄存器的操作//////////////////////////////////
/*  读取RC522指定寄存器的值
    向RC522指定寄存器中写入指定的数据
    置位RC522指定寄存器的指定位
    清位RC522指定寄存器的指定位
*/

/**
  * @brief  ：读取RC522指定寄存器的值
        * @param  ：Address:寄存器的地址
  * @retval ：寄存器的值
*/
uint8_t RC522_Read_Register( uint8_t Address )
{
        uint8_t data,Addr;

        Addr = ( (Address<<1)&0x7E )|0x80;

        RC522_CS_Enable();
        RC522_SPI_SendByte( Addr );
        data = RC522_SPI_ReadByte();//读取寄存器中的值
        RC522_CS_Disable();

        return data;
}

/**
  * @brief  ：向RC522指定寄存器中写入指定的数据
  * @param  ：Address：寄存器地址
                                      data：要写入寄存器的数据
  * @retval ：无
*/
void RC522_Write_Register( uint8_t Address, uint8_t data )
{
        uint8_t Addr;

        Addr = ( Address<<1 )&0x7E;

        RC522_CS_Enable();
        RC522_SPI_SendByte( Addr );
        RC522_SPI_SendByte( data );
        RC522_CS_Disable();

}

/**
  * @brief  ：置位RC522指定寄存器的指定位
  * @param  ：Address：寄存器地址
                                      mask：置位值
  * @retval ：无
*/
void RC522_SetBit_Register( uint8_t Address, uint8_t mask )
{
        uint8_t temp;
        /* 获取寄存器当前值 */
        temp = RC522_Read_Register( Address );
        /* 对指定位进行置位操作后，再将值写入寄存器 */
        RC522_Write_Register( Address, temp|mask );
}

/**
  * @brief  ：清位RC522指定寄存器的指定位
  * @param  ：Address：寄存器地址
                      mask：清位值
  * @retval ：无
*/
void RC522_ClearBit_Register( uint8_t Address, uint8_t mask )
{
        uint8_t temp;
        /* 获取寄存器当前值 */
        temp = RC522_Read_Register( Address );
        /* 对指定位进行清位操作后，再将值写入寄存器 */
        RC522_Write_Register( Address, temp&(~mask) );
}

///////////////////GD32对RC522的基础通信///////////////////////////////////
/*
    开启天线
    关闭天线
    复位RC522
    设置RC522工作方式
*/

/**
  * @brief  ：开启天线
  * @param  ：无
  * @retval ：无
*/
void RC522_Antenna_On( void )
{
        uint8_t k;
        k = RC522_Read_Register( TxControlReg );
        /* 判断天线是否开启 */
        if( !( k&0x03 ) )
                RC522_SetBit_Register( TxControlReg, 0x03 );
}

/**
  * @brief  ：关闭天线
  * @param  ：无
  * @retval ：无
*/
void RC522_Antenna_Off( void )
{
        /* 直接对相应位清零 */
        RC522_ClearBit_Register( TxControlReg, 0x03 );
}

/**
  * @brief  ：复位RC522
  * @param  ：无
  * @retval ：无
*/
void RC522_Rese( void )
{
        RC522_Reset_Disable();
        delay_us ( 1 );
        RC522_Reset_Enable();
        delay_us ( 1 );
        RC522_Reset_Disable();
        delay_us ( 1 );
        RC522_Write_Register( CommandReg, 0x0F );
        while( RC522_Read_Register( CommandReg )&0x10 );

        /* 缓冲一下 */
        delay_us ( 1 );
        RC522_Write_Register( ModeReg, 0x3D );       //定义发送和接收常用模式
        RC522_Write_Register( TReloadRegL, 30 );     //16位定时器低位
        RC522_Write_Register( TReloadRegH, 0 );      //16位定时器高位
        RC522_Write_Register( TModeReg, 0x8D );      //内部定时器的设置
        RC522_Write_Register( TPrescalerReg, 0x3E ); //设置定时器分频系数
        RC522_Write_Register( TxAutoReg, 0x40 );     //调制发送信号为100%ASK

        RC522_SetBit_Register(GsNReg, 0xff);
        RC522_SetBit_Register(CWGsCfgReg, 0x3f);
        RC522_SetBit_Register(ModGsCfgReg, 0x3f);
        RC522_SetBit_Register(RFCfgReg,0x7f);//接收增益调到最大
}

/**
  * @brief  ：设置RC522的工作方式
  * @param  ：Type：工作方式
  * @retval ：无
  M500PcdConfigISOType
*/
void RC522_Config_Type( char Type )
{
        if( Type=='A' )
        {
                RC522_ClearBit_Register( Status2Reg, 0x08 );
                RC522_Write_Register( ModeReg, 0x3D );
                RC522_Write_Register( RxSelReg, 0x86 );
                RC522_Write_Register( RFCfgReg, 0x7F );
                RC522_Write_Register( TReloadRegL, 30 );
                RC522_Write_Register( TReloadRegH, 0 );
                RC522_Write_Register( TModeReg, 0x8D );
                RC522_Write_Register( TPrescalerReg, 0x3E );
                delay_us(2);
                /* 开天线 */
                RC522_Antenna_On();
        }
}

/////////////////////////GD32控制RC522与M1卡的通信///////////////////////////////////////
/*
    通过RC522和M1卡通讯（数据的双向传输）
    寻卡
    防冲突
    用RC522计算CRC16（循环冗余校验）
    选定卡片
    校验卡片密码
    在M1卡的指定块地址写入指定数据
    读取M1卡的指定块地址的数据
    让卡片进入休眠模式
*/

/**
  * @brief  ：通过RC522和ISO14443卡通讯
* @param  ：ucCommand：RC522命令字
 *          pInData：通过RC522发送到卡片的数据
 *          ucInLenByte：发送数据的字节长度
 *          pOutData：接收到的卡片返回数据
 *          pOutLenBit：返回数据的位长度
  * @retval ：状态值MI_OK，成功
*/
char PcdComMF522 ( uint8_t ucCommand, uint8_t * pInData, uint8_t ucInLenByte, uint8_t * pOutData, uint32_t * pOutLenBit )
{
    char cStatus = MI_ERR;
    uint8_t ucIrqEn   = 0x00;
    uint8_t ucWaitFor = 0x00;
    uint8_t ucLastBits;
    uint8_t ucN;
    uint32_t ul;


    switch ( ucCommand )
    {
       case PCD_AUTHENT:                //Mifare认证
          ucIrqEn   = 0x12;                //允许错误中断请求ErrIEn  允许空闲中断IdleIEn
          ucWaitFor = 0x10;                //认证寻卡等待时候 查询空闲中断标志位
          break;

       case PCD_TRANSCEIVE:                //接收发送 发送接收
          ucIrqEn   = 0x77;                //允许TxIEn RxIEn IdleIEn LoAlertIEn ErrIEn TimerIEn
          ucWaitFor = 0x30;                //寻卡等待时候 查询接收中断标志位与 空闲中断标志位
          break;

       default:
         break;

    }

    RC522_Write_Register ( ComIEnReg, ucIrqEn | 0x80 );                //IRqInv置位管脚IRQ与Status1Reg的IRq位的值相反
    RC522_ClearBit_Register ( ComIrqReg, 0x80 );                        //Set1该位清零时，CommIRqReg的屏蔽位清零
    RC522_Write_Register ( CommandReg, PCD_IDLE );                //写空闲命令
    RC522_SetBit_Register ( FIFOLevelReg, 0x80 );                        //置位FlushBuffer清除内部FIFO的读和写指针以及ErrReg的BufferOvfl标志位被清除

    for ( ul = 0; ul < ucInLenByte; ul ++ )
                  RC522_Write_Register ( FIFODataReg, pInData [ ul ] );                    //写数据进FIFOdata

    RC522_Write_Register ( CommandReg, ucCommand );                                        //写命令


    if ( ucCommand == PCD_TRANSCEIVE )
                        RC522_SetBit_Register(BitFramingReg,0x80);                                  //StartSend置位启动数据发送 该位与收发命令使用时才有效

    ul = 5000;//根据时钟频率调整，操作M1卡最大等待时间25ms

    do                                                                                                                 //认证 与寻卡等待时间
    {
         ucN = RC522_Read_Register ( ComIrqReg );                                                        //查询事件中断
         ul --;
    } while ( ( ul != 0 ) && ( ! ( ucN & 0x01 ) ) && ( ! ( ucN & ucWaitFor ) ) );                //退出条件i=0,定时器中断，与写空闲命令
    // uint8_t com_irq = ucN;
    // uint8_t err_reg = RC522_Read_Register(ErrorReg);
    // ESP_LOGW("RC522_DBG", "PcdComMF522: ComIrqReg=0x%02X, ErrorReg=0x%02X, FIFOLevel=%d",
    //      com_irq, err_reg, RC522_Read_Register(FIFOLevelReg));
    RC522_ClearBit_Register ( BitFramingReg, 0x80 );                                        //清理允许StartSend位

    if ( ul != 0 )
    {
                        if ( ! ( RC522_Read_Register ( ErrorReg ) & 0x1B ) )                        //读错误标志寄存器BufferOfI CollErr ParityErr ProtocolErr
                        {
                                cStatus = MI_OK;

                                if ( ucN & ucIrqEn & 0x01 )                                        //是否发生定时器中断
                                  cStatus = MI_NOTAGERR;

                                if ( ucCommand == PCD_TRANSCEIVE )
                                {
                                        ucN = RC522_Read_Register ( FIFOLevelReg );                        //读FIFO中保存的字节数

                                        ucLastBits = RC522_Read_Register ( ControlReg ) & 0x07;        //最后接收到得字节的有效位数

                                        if ( ucLastBits )
                                                * pOutLenBit = ( ucN - 1 ) * 8 + ucLastBits;           //N个字节数减去1（最后一个字节）+最后一位的位数 读取到的数据总位数
                                        else
                                                * pOutLenBit = ucN * 8;                                           //最后接收到的字节整个字节有效

                                        if ( ucN == 0 )
            ucN = 1;

                                        if ( ucN > MAXRLEN )
                                                ucN = MAXRLEN;

                                        for ( ul = 0; ul < ucN; ul ++ )
                                          pOutData [ ul ] = RC522_Read_Register ( FIFODataReg );
                                        }
      }
                        else
                                cStatus = MI_ERR;
    }

   RC522_SetBit_Register ( ControlReg, 0x80 );           // stop timer now
   RC522_Write_Register ( CommandReg, PCD_IDLE );

   return cStatus;
}

/**
  * @brief  ：寻卡
* @param  ucReq_code，寻卡方式
*                      = 0x52：寻感应区内所有符合14443A标准的卡
 *                     = 0x26：寻未进入休眠状态的卡
 *         pTagType，卡片类型代码
 *                   = 0x4400：Mifare_UltraLight
 *                   = 0x0400：Mifare_One(S50)
 *                   = 0x0200：Mifare_One(S70)
 *                   = 0x0800：Mifare_Pro(X))
 *                   = 0x4403：Mifare_DESFire
  * @retval ：状态值MI_OK，成功
*/
char PcdRequest ( uint8_t ucReq_code, uint8_t * pTagType )
{
   char cStatus;
   uint8_t ucComMF522Buf [ MAXRLEN ];
   uint32_t ulLen;

   RC522_ClearBit_Register ( Status2Reg, 0x08 );        //清理指示MIFARECyptol单元接通以及所有卡的数据通信被加密的情况
   RC522_Write_Register ( BitFramingReg, 0x07 );        //        发送的最后一个字节的 七位
   RC522_SetBit_Register ( TxControlReg, 0x03 );        //TX1,TX2管脚的输出信号传递经发送调制的13.56的能量载波信号

   ucComMF522Buf [ 0 ] = ucReq_code;                //存入寻卡方式
        /* PCD_TRANSCEIVE：发送并接收数据的命令，RC522向卡片发送寻卡命令，卡片返回卡的型号代码到ucComMF522Buf中 */
   cStatus = PcdComMF522 ( PCD_TRANSCEIVE,        ucComMF522Buf, 1, ucComMF522Buf, & ulLen );        //寻卡

   if ( ( cStatus == MI_OK ) && ( ulLen == 0x10 ) )        //寻卡成功返回卡类型
   {
                 /* 接收卡片的型号代码 */
       * pTagType = ucComMF522Buf [ 0 ];
       * ( pTagType + 1 ) = ucComMF522Buf [ 1 ];
   }
   else
   {
        cStatus = MI_ERR;
   }
    return cStatus;
}

/**
  * @brief  ：防冲突
        * @param  ：Snr：卡片序列，4字节，会返回选中卡片的序列
  * @retval ：状态值MI_OK，成功
*/
char PcdAnticoll ( uint8_t * pSnr )
{
    char cStatus;
    uint8_t uc, ucSnr_check = 0;
    uint8_t ucComMF522Buf [ MAXRLEN ];
          uint32_t ulLen;

    RC522_ClearBit_Register ( Status2Reg, 0x08 );                //清MFCryptol On位 只有成功执行MFAuthent命令后，该位才能置位
    RC522_Write_Register ( BitFramingReg, 0x00);                //清理寄存器 停止收发
    RC522_ClearBit_Register ( CollReg, 0x80 );                        //清ValuesAfterColl所有接收的位在冲突后被清除

    ucComMF522Buf [ 0 ] = 0x93;        //卡片防冲突命令
    ucComMF522Buf [ 1 ] = 0x20;

          /* 将卡片防冲突命令通过RC522传到卡片中，返回的是被选中卡片的序列 */
    cStatus = PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 2, ucComMF522Buf, & ulLen);//与卡片通信

    if ( cStatus == MI_OK)                //通信成功
    {
                        for ( uc = 0; uc < 4; uc ++ )
                        {
         * ( pSnr + uc )  = ucComMF522Buf [ uc ];                        //读出UID
         ucSnr_check ^= ucComMF522Buf [ uc ];
      }

      if ( ucSnr_check != ucComMF522Buf [ uc ] )
                                cStatus = MI_ERR;
    }
    RC522_SetBit_Register ( CollReg, 0x80 );
    return cStatus;
}

/**
 * @brief   :用RC522计算CRC16（循环冗余校验）
        * @param  ：pIndata：计算CRC16的数组
 *            ucLen：计算CRC16的数组字节长度
 *            pOutData：存放计算结果存放的首地址
  * @retval ：状态值MI_OK，成功
*/
void CalulateCRC ( uint8_t * pIndata, u8 ucLen, uint8_t * pOutData )
{
    uint8_t uc, ucN;


    RC522_ClearBit_Register(DivIrqReg,0x04);
    RC522_Write_Register(CommandReg,PCD_IDLE);
    RC522_SetBit_Register(FIFOLevelReg,0x80);

    for ( uc = 0; uc < ucLen; uc ++)
            RC522_Write_Register ( FIFODataReg, * ( pIndata + uc ) );

    RC522_Write_Register ( CommandReg, PCD_CALCCRC );

    uc = 0xFF;

    do
    {
        ucN = RC522_Read_Register ( DivIrqReg );
        uc --;
    } while ( ( uc != 0 ) && ! ( ucN & 0x04 ) );

    pOutData [ 0 ] = RC522_Read_Register ( CRCResultRegL );
    pOutData [ 1 ] = RC522_Read_Register ( CRCResultRegM );

}

/**
  * @brief   :选定卡片
  * @param  ：pSnr：卡片序列号，4字节
  * @retval ：状态值MI_OK，成功
*/
char PcdSelect ( uint8_t * pSnr )
{
    char ucN;
    uint8_t uc;
          uint8_t ucComMF522Buf [ MAXRLEN ];
    uint32_t  ulLen;
    /* PICC_ANTICOLL1：防冲突命令 */
    ucComMF522Buf [ 0 ] = PICC_ANTICOLL1;
    ucComMF522Buf [ 1 ] = 0x70;
    ucComMF522Buf [ 6 ] = 0;

    for ( uc = 0; uc < 4; uc ++ )
    {
            ucComMF522Buf [ uc + 2 ] = * ( pSnr + uc );
            ucComMF522Buf [ 6 ] ^= * ( pSnr + uc );
    }

    CalulateCRC ( ucComMF522Buf, 7, & ucComMF522Buf [ 7 ] );

    RC522_ClearBit_Register ( Status2Reg, 0x08 );

    ucN = PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 9, ucComMF522Buf, & ulLen );

    if ( ( ucN == MI_OK ) && ( ulLen == 0x18 ) )
      ucN = MI_OK;
    else
      ucN = MI_ERR;

    return ucN;

}

/**
  * @brief   :校验卡片密码
  * @param  ：ucAuth_mode：密码验证模式
  *                     = 0x60，验证A密钥
  *                     = 0x61，验证B密钥
  *           ucAddr：块地址
  *           pKey：密码
  *           pSnr：卡片序列号，4字节
  * @retval ：状态值MI_OK，成功
*/
char PcdAuthState ( uint8_t ucAuth_mode, uint8_t ucAddr, uint8_t * pKey, uint8_t * pSnr )
{
    char cStatus;
          uint8_t uc, ucComMF522Buf [ MAXRLEN ];
    uint32_t ulLen;

    ucComMF522Buf [ 0 ] = ucAuth_mode;
    ucComMF522Buf [ 1 ] = ucAddr;
          /* 前俩字节存储验证模式和块地址，2~8字节存储密码（6个字节），8~14字节存储序列号 */
    for ( uc = 0; uc < 6; uc ++ )
            ucComMF522Buf [ uc + 2 ] = * ( pKey + uc );

    for ( uc = 0; uc < 6; uc ++ )
            ucComMF522Buf [ uc + 8 ] = * ( pSnr + uc );
    /* 进行冗余校验，14~16俩个字节存储校验结果 */
    cStatus = PcdComMF522 ( PCD_AUTHENT, ucComMF522Buf, 12, ucComMF522Buf, & ulLen );
          /* 判断验证是否成功 */
    if ( ( cStatus != MI_OK ) || ( ! ( RC522_Read_Register ( Status2Reg ) & 0x08 ) ) )
      cStatus = MI_ERR;

    return cStatus;

}

/**
  * @brief   :在M1卡的指定块地址写入指定数据
  * @param  ：ucAddr：块地址
  *           pData：写入的数据，16字节
  * @retval ：状态值MI_OK，成功
*/
char PcdWrite ( uint8_t ucAddr, uint8_t * pData )
{
    char cStatus;
          uint8_t uc, ucComMF522Buf [ MAXRLEN ];
    uint32_t ulLen;

    ucComMF522Buf [ 0 ] = PICC_WRITE;//写块命令
    ucComMF522Buf [ 1 ] = ucAddr;//写块地址

          /* 进行循环冗余校验，将结果存储在& ucComMF522Buf [ 2 ] */
    CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );

        /* PCD_TRANSCEIVE:发送并接收数据命令，通过RC522向卡片发送写块命令 */
    cStatus = PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 4, ucComMF522Buf, & ulLen );

                /* 通过卡片返回的信息判断，RC522是否与卡片正常通信 */
    if ( ( cStatus != MI_OK ) || ( ulLen != 4 ) || ( ( ucComMF522Buf [ 0 ] & 0x0F ) != 0x0A ) )
      cStatus = MI_ERR;

    if ( cStatus == MI_OK )
    {
                        //memcpy(ucComMF522Buf, pData, 16);
                        /* 将要写入的16字节的数据，传入ucComMF522Buf数组中 */
      for ( uc = 0; uc < 16; uc ++ )
                          ucComMF522Buf [ uc ] = * ( pData + uc );
                        /* 冗余校验 */
      CalulateCRC ( ucComMF522Buf, 16, & ucComMF522Buf [ 16 ] );
      /* 通过RC522，将16字节数据包括2字节校验结果写入卡片中 */
      cStatus = PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 18, ucComMF522Buf, & ulLen );
                        /* 判断写地址是否成功 */
                        if ( ( cStatus != MI_OK ) || ( ulLen != 4 ) || ( ( ucComMF522Buf [ 0 ] & 0x0F ) != 0x0A ) )
        cStatus = MI_ERR;
    }
    return cStatus;
}

/**
  * @brief   :读取M1卡的指定块地址的数据
  * @param  ：ucAddr：块地址
  *           pData：读出的数据，16字节
  * @retval ：状态值MI_OK，成功
*/
char PcdRead ( uint8_t ucAddr, uint8_t * pData )
{
    char cStatus;
          uint8_t uc, ucComMF522Buf [ MAXRLEN ];
    uint32_t ulLen;

    ucComMF522Buf [ 0 ] = PICC_READ;
    ucComMF522Buf [ 1 ] = ucAddr;
          /* 冗余校验 */
    CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );
    /* 通过RC522将命令传给卡片 */
    cStatus = PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 4, ucComMF522Buf, & ulLen );

          /* 如果传输正常，将读取到的数据传入pData中 */
    if ( ( cStatus == MI_OK ) && ( ulLen == 0x90 ) )
    {
                        for ( uc = 0; uc < 16; uc ++ )
        * ( pData + uc ) = ucComMF522Buf [ uc ];
    }
    else
      cStatus = MI_ERR;

    return cStatus;

}

/**
  * @brief   :让卡片进入休眠模式
  * @param  ：无
  * @retval ：状态值MI_OK，成功
*/
char PcdHalt( void )
{
        uint8_t ucComMF522Buf [ MAXRLEN ];
        uint32_t  ulLen;
  char ret;
  ucComMF522Buf [ 0 ] = PICC_HALT;
  ucComMF522Buf [ 1 ] = 0;

  CalulateCRC ( ucComMF522Buf, 2, & ucComMF522Buf [ 2 ] );
         ret =PcdComMF522 ( PCD_TRANSCEIVE, ucComMF522Buf, 4, ucComMF522Buf, & ulLen );

  return ret;

}

void RC522_ForceReceiverOff(void)
{
ESP_LOGI("RC522", "精确关闭接收器");
    
    // 方法1：直接设置CommandReg的RcvOff位（位5）
    // 这是最直接的方法，专门用于关闭接收器模拟电路
    uint8_t cmd_reg = RC522_Read_Register(CommandReg);
    ESP_LOGI("RC522", "原始CommandReg: 0x%02X", cmd_reg);
    
    // 设置RcvOff位（bit5=1），同时保持其他位不变
    cmd_reg |= 0x20;  // 0x20 = 0010 0000，设置第5位
    RC522_Write_Register(CommandReg, cmd_reg);
    delay_ms(2);  // 等待寄存器更新
    
    // 验证设置
    cmd_reg = RC522_Read_Register(CommandReg);
    ESP_LOGI("RC522", "设置后CommandReg: 0x%02X", cmd_reg);
    
    if ((cmd_reg & 0x20) == 0x20) {
        ESP_LOGW("RC522", "✅ 接收器已成功关闭 (RcvOff=1)");
    } else {
        ESP_LOGE("RC522", "❌ 接收器关闭失败");
    }
    
    // 可选：同时关闭接收相关中断，防止误触发
    RC522_ClearBit_Register(ComIEnReg, 0x20);  // 清除RxIEn位（位5）
    
    // 可选：降低接收增益进一步省电
    RC522_Write_Register(RFCfgReg, 0x00);  // 将接收增益设为零
}
void check_rc522_low_power_status(void)
{
    ESP_LOGI("DEBUG", "=== RC522低功耗状态检查 ===");
    
    // 先检查NRSTPD引脚状态
    int rst_state = gpio_get_level(GPIO_RST);
    ESP_LOGI("DEBUG", "NRSTPD引脚电平: %s", rst_state ? "高电平" : "低电平");
    
    // 如果NRSTPD为低电平（硬掉电状态），给出警告但不读取寄存器
    if (rst_state == 0) {
        ESP_LOGI("DEBUG", "警告：硬掉电模式下，寄存器读取不可靠");
        ESP_LOGI("DEBUG", "跳过寄存器检查以避免随机值");
        ESP_LOGI("DEBUG", "=== 检查完成 ===");
        return;
    }
    
    // NRSTPD为高电平，正常读取寄存器
    // 1. 检查PowerDown命令是否生效
    uint8_t cmd_status = RC522_Read_Register(CommandReg);
    ESP_LOGI("DEBUG", "CommandReg (0x01): 0x%02X", cmd_status);
    ESP_LOGI("DEBUG", "  - bit5 (RcvOff): %d [%s]", 
             (cmd_status>>5)&1, (cmd_status & 0x20) ? "接收器关闭" : "接收器开启");
    ESP_LOGI("DEBUG", "  - bit4 (PowerDown): %d [%s]", 
             (cmd_status>>4)&1, (cmd_status & 0x10) ? "PowerDown已生效" : "PowerDown未生效");
    
    // 2. 检查接收器状态
    uint8_t rx_mode = RC522_Read_Register(RxModeReg);
    ESP_LOGI("DEBUG", "RxModeReg (0x13): 0x%02X", rx_mode);
    
    // 3. 检查天线控制
    uint8_t tx_ctrl = RC522_Read_Register(TxControlReg);
    ESP_LOGI("DEBUG", "TxControlReg (0x14): 0x%02X [天线驱动器: %s]", 
             tx_ctrl, (tx_ctrl & 0x03) ? "开启" : "关闭");
    
    // 4. 检查版本寄存器（确认芯片正常）
    uint8_t version = RC522_Read_Register(VersionReg);
    ESP_LOGI("DEBUG", "VersionReg (0x37): 0x%02X", version);
    
    ESP_LOGI("DEBUG", "=== 检查完成 ===");
}
/**
  * @brief   : 让RC522芯片进入低功耗模式（PowerDown）
  * @param   : 无
  * @retval  : MI_OK成功，MI_ERR失败
  * @note    : 进入低功耗模式前应确保天线已关闭
*/
char PcdPowerDown(void)
{
ESP_LOGI("RC522", "进入PowerDown低功耗模式");
    
    // 第一步：强制停止所有操作
    RC522_Write_Register(CommandReg, PCD_IDLE);
    delay_ms(1);
    
    // 第二步：关闭所有模拟电路
    RC522_Write_Register(TxControlReg, 0x00);   // 关闭天线驱动器
    RC522_Write_Register(TxASKReg, 0x00);       // 关闭ASK调制
    RC522_Write_Register(TxModeReg, 0x00);      // 发射器关闭
    RC522_Write_Register(RxModeReg, 0x00);      // 接收器关闭
    
    // 第三步：设置RcvOff位关闭接收器
    uint8_t current_cmd = RC522_Read_Register(CommandReg);
    uint8_t new_cmd = current_cmd | 0x20;  // 设置RcvOff位（bit5）
    RC522_Write_Register(CommandReg, new_cmd);
    delay_ms(1);
    
    // 第四步：进入PowerDown模式
    RC522_Write_Register(CommandReg, 0x10);     // PowerDown位=1
    delay_ms(5);
    
    // 第五步：检查配置结果（仅在NRSTPD为高时）
    check_rc522_low_power_status();  // ✅ 正确的调用方式
    
    uint8_t cmd_status = RC522_Read_Register(CommandReg);
    ESP_LOGI("RC522", "PowerDown后CommandReg: 0x%02X", cmd_status);
    
    // 第六步：降低SPI引脚功耗
    gpio_set_direction(GPIO_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_CS, 0);
    
    gpio_set_direction(GPIO_SCK, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SCK, 0);
    
    gpio_set_direction(GPIO_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_MOSI, 0);
    
    gpio_set_direction(GPIO_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_MISO, GPIO_FLOATING);

    if ((cmd_status & 0x10) == 0x10) {
        ESP_LOGI("RC522", "PowerDown低功耗配置成功");
        return MI_OK;
    } else {
        ESP_LOGE("RC522", "PowerDown低功耗配置失败");
        return MI_ERR;
    }
}

char PcdHardPowerDown(void)
{
    ESP_LOGI("RC522", "进入硬掉电模式");
    
    // 1. 强制关闭接收器（解决RxModeReg问题）
    RC522_ForceReceiverOff();
    delay_ms(2);
    
    // 2. 执行软掉电
    char power_down_result = PcdPowerDown();
    
    // 3. 即使软掉电报告失败，也继续执行硬掉电（强制关闭）
    if (power_down_result != MI_OK) {
        ESP_LOGW("RC522", "软掉电报告失败，但继续执行硬掉电强制关闭");
    }
    
    // 4. 确保所有SPI通信完成
    delay_ms(5);
    
    // 5. 拉低NRSTPD引脚，进入硬掉电模式
    ESP_LOGI("RC522", "拉低NRSTPD引脚...");
    RC522_Reset_Enable();  // 拉低NRSTPD并保持
    
    // 6. 验证NRSTPD引脚状态
    delay_ms(1);
    int rst_state = gpio_get_level(GPIO_RST);
    
    if (rst_state == 0) {
        ESP_LOGI("RC522", "硬掉电模式已激活，NRSTPD引脚保持低电平");
        return MI_OK;
    } else {
        ESP_LOGE("RC522", "硬掉电模式失败，NRSTPD引脚仍为高电平");
        return MI_ERR;
    }
}


// NTAG21x 命令
#define NTAG_CMD_READ           0x30    // 读4页
#define NTAG_CMD_FAST_READ      0x3A    // 读多页
#define NTAG_CMD_WRITE          0xA2    // 写1页
#define NTAG_CMD_GET_VERSION    0x60    // 获取版本
#define NTAG_CMD_READ_CNT       0x39    // 读NFC计数器
#define NTAG_CMD_PWD_AUTH       0x1B    // 密码验证
#define NTAG_CMD_READ_SIG       0x3C    // 读原始签名

/**
 * @brief  : NTAG21x 防冲突与选择（获取完整7字节UID）
 * @param  : pSnr: 输出7字节UID缓冲区
 * @retval : MI_OK 成功，MI_ERR 失败
 */
char PcdNTAG21xAnticollSelect(uint8_t *pSnr)
{
    char status;
    uint8_t buf[MAXRLEN];
    uint32_t len;
    uint8_t sak;

    // 初始化寄存器（与 PcdAnticoll 一致）
    RC522_ClearBit_Register(Status2Reg, 0x08);
    RC522_Write_Register(BitFramingReg, 0x00);
    RC522_ClearBit_Register(CollReg, 0x80);

    // 级联级别1 (CL1) 防冲突
    buf[0] = 0x93;
    buf[1] = 0x20;
    ESP_LOGD("RC522", "Sending CL1 anticoll: %02X %02X", buf[0], buf[1]);
    status = PcdComMF522(PCD_TRANSCEIVE, buf, 2, buf, &len);
    ESP_LOGD("RC522", "CL1 anticoll: status=%d, len=%d bits", status, len);
    if (len >= 40) {
        ESP_LOGD("RC522", "  data: %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4]);
    }
    if (status != MI_OK || len != 40) return MI_ERR;

    uint8_t cl1_uid[4] = {buf[0], buf[1], buf[2], buf[3]};
    uint8_t bcc0 = buf[4];

    // 级联选择 CL1
    buf[0] = 0x93;
    buf[1] = 0x70;
    memcpy(&buf[2], cl1_uid, 4);
    buf[6] = bcc0;
    CalulateCRC(buf, 7, &buf[7]);
    ESP_LOGD("RC522", "Sending CL1 select: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
    status = PcdComMF522(PCD_TRANSCEIVE, buf, 9, buf, &len);
    ESP_LOGD("RC522", "CL1 select: status=%d, len=%d bits, SAK=0x%02X", status, len, buf[0]);
    if (status != MI_OK || len != 24) return MI_ERR; // SAK(1) + CRC(2) = 24 bits

    sak = buf[0];

    if (sak & 0x04) {
        ESP_LOGD("RC522", "SAK indicates cascade level 2, proceeding to CL2...");

        // 保存 CL1 返回的真正 UID 前三个字节（跳过 CT）
        uint8_t uid0 = cl1_uid[1];  // buf[1] = 04
        uint8_t uid1 = cl1_uid[2];  // buf[2] = E2
        uint8_t uid2 = cl1_uid[3];  // buf[3] = 42

        // 再次初始化寄存器
        RC522_Write_Register(BitFramingReg, 0x00);
        RC522_ClearBit_Register(CollReg, 0x80);

        // 级联级别2 (CL2) 防冲突
        buf[0] = 0x95;
        buf[1] = 0x20;
        ESP_LOGD("RC522", "Sending CL2 anticoll: %02X %02X", buf[0], buf[1]);
        status = PcdComMF522(PCD_TRANSCEIVE, buf, 2, buf, &len);
        ESP_LOGD("RC522", "CL2 anticoll: status=%d, len=%d bits", status, len);
        if (len >= 40) {
            ESP_LOGD("RC522", "  data: %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4]);
        }
        if (status != MI_OK || len != 40) return MI_ERR;

        // 组合完整7字节UID
        pSnr[0] = uid0;
        pSnr[1] = uid1;
        pSnr[2] = uid2;
        pSnr[3] = buf[0];  // A2
        pSnr[4] = buf[1];  // F8
        pSnr[5] = buf[2];  // 11
        pSnr[6] = buf[3];  // 90
        uint8_t bcc1 = buf[4];

        // 级联选择 CL2
        buf[0] = 0x95;
        buf[1] = 0x70;
        buf[2] = pSnr[3];
        buf[3] = pSnr[4];
        buf[4] = pSnr[5];
        buf[5] = pSnr[6];
        buf[6] = bcc1;
        CalulateCRC(buf, 7, &buf[7]);
        ESP_LOGD("RC522", "Sending CL2 select: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
        status = PcdComMF522(PCD_TRANSCEIVE, buf, 9, buf, &len);
        ESP_LOGD("RC522", "CL2 select: status=%d, len=%d bits", status, len);
        if (status != MI_OK || len != 24) return MI_ERR;
    } else {
        ESP_LOGD("RC522", "No cascade needed, UID length 4 bytes");
        memcpy(pSnr, cl1_uid, 4);
    }
    return MI_OK;}







/**
 * 读取NTAG21x单页，带重试和超时增强
 * @param page_addr 页地址
 * @param out_4bytes 输出4字节缓冲区
 * @return MI_OK 成功，MI_ERR 失败
 */
char NTAG21x_ReadSinglePage(uint8_t page_addr, uint8_t *out_4bytes) {
    const int max_retry = 5;
    for (int retry = 0; retry < max_retry; retry++) {
        uint8_t cmd[4] = {0x30, page_addr};
        uint8_t resp[32];
        uint32_t resp_bits;
        CalulateCRC(cmd, 2, &cmd[2]);

        char status = PcdComMF522(PCD_TRANSCEIVE, cmd, 4, resp, &resp_bits);
        if (status == MI_OK && resp_bits == 144) { // 144 bits = 16数据 + 2CRC
            uint8_t offset = (page_addr & 0x03) * 4;
            memcpy(out_4bytes, resp + offset, 4);
            return MI_OK;
        }
        ESP_LOGD("NTAG21x", "Read page %d failed (retry %d), status=%d, bits=%lu",
                 page_addr, retry, status, (unsigned long)resp_bits);
        delay_ms(20); // 重试前等待
    }
    return MI_ERR;
}


/**
 * 使用FAST_READ命令一次性读取从start_page到end_page的所有页
 * @param start_page 起始页
 * @param end_page   结束页（包含）
 * @param out_data   输出缓冲区，大小至少 (end_page-start_page+1)*4
 * @param out_len    返回实际字节数
 * @param max_retry  最大重试次数
 * @return MI_OK 成功，MI_ERR 失败
 */
char NTAG21x_FastRead(uint8_t start_page, uint8_t end_page, uint8_t *out_data, uint16_t *out_len, int max_retry) {
    for (int retry = 0; retry < max_retry; retry++) {
        uint8_t cmd[5] = {0x3A, start_page, end_page};
        uint8_t resp[1024]; // 足够容纳最大响应
        uint32_t resp_bits;
        CalulateCRC(cmd, 3, &cmd[3]);

        char status = PcdComMF522(PCD_TRANSCEIVE, cmd, 5, resp, &resp_bits);
        if (status == MI_OK) {
            uint16_t expected_bits = (end_page - start_page + 1) * 4 * 8 + 16; // 数据+2字节CRC
            if (resp_bits == expected_bits) {
                uint16_t data_bytes = (end_page - start_page + 1) * 4;
                memcpy(out_data, resp, data_bytes);
                *out_len = data_bytes;
                return MI_OK;
            } else {
                ESP_LOGD("NTAG21x", "FAST_READ length mismatch: got %lu bits, expected %d", (unsigned long)resp_bits, expected_bits);
            }
        }
        ESP_LOGD("NTAG21x", "FAST_READ failed (retry %d), status=%d", retry, status);
        delay_ms(20);
    }
    return MI_ERR;
}




/**
 * 稳定读取 NTAG21x 全部用户内存（页 0x04~0x27），每次最多读 4 页，失败重试直到成功
 * @param out_data 输出缓冲区（至少 144 字节）
 * @param out_len 返回实际读取字节数
 * @param max_retry_per_segment 每段最大重试次数
 * @return MI_OK 成功，MI_ERR 失败（超过重试）
 */
char NTAG21x_ReadStableUserMemory(uint8_t *out_data, uint16_t *out_len, int max_retry_per_segment) {
    const uint8_t start_page = 0x04;
    const uint8_t end_page = 0x27;
    const uint8_t step = 4;  // 每次最多读 4 页
    uint8_t page = start_page;
    uint16_t offset = 0;

    while (page <= end_page) {
        uint8_t to = page + step - 1;
        if (to > end_page) to = end_page;

        uint8_t buf[16];  // 最多 4 页 = 16 字节
        uint16_t len = 0;
        int retry = 0;
        char status = MI_ERR;

        while (retry < max_retry_per_segment) {
            status = NTAG21x_FastRead(page, to, buf, &len, 1);  // 内部已重试，这里再外层重试
            if (status == MI_OK && len == (to - page + 1) * 4) {
                break;
            }
            ESP_LOGD("NTAG21x", "Read pages %d-%d failed (retry %d), status=%d", page, to, retry, status);
            retry++;
            delay_ms(20);
        }

        if (status != MI_OK) {
            ESP_LOGD("NTAG21x", "Failed to read pages %d-%d after %d retries", page, to, max_retry_per_segment);
            return MI_ERR;
        }

        memcpy(out_data + offset, buf, len);
        offset += len;
        page = to + 1;
        delay_ms(5);  // 页间延时
    }

    *out_len = offset;
    ESP_LOGI("NTAG21x", "Stable read success, total %d bytes", offset);
    return MI_OK;
}

/**
 * 从用户内存中提取以 "avery" 开头的连续 ASCII 字符串
 * @param data 用户内存数据
 * @param len 数据长度
 * @param out_str 输出缓冲区（至少 64 字节）
 * @param out_len 返回字符串长度
 * @return 1 成功，0 失败
 */
int extract_avery_string(uint8_t *data, uint16_t len, char *out_str, uint16_t *out_len) {
    const uint8_t header[] = {'a', 'v', 'e', 'r', 'y'};
    for (uint16_t i = 0; i <= len - 5; i++) {
        if (memcmp(data + i, header, 5) == 0) {
            // 找到起始码，开始提取后续可打印 ASCII，直到遇到 0x00、0xFE 或不可打印
            uint16_t j = i;
            uint16_t str_len = 0;
            while (j < len && data[j] >= 0x20 && data[j] <= 0x7E && data[j] != 0xFE) {
                out_str[str_len++] = data[j++];
            }
            out_str[str_len] = '\0';
            *out_len = str_len;
            ESP_LOGI("EXTRACT", "Found header at offset %d, extracted: %s", i, out_str);
            return 1;
        }
    }
    ESP_LOGE("EXTRACT", "Header 'avery' not found");
    return 0;
}


// CRC-16/CCITT 计算（多项式 0x1021，初始值 0xFFFF）
static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}
/**
 * 解析 RFID 数据包（22字节，含起始码 "avery"）
 * @param packet 指向22字节数据的指针（必须从 'a' 开始）
 * @param fields 输出解析后的字段
 * @return 1成功，0失败
 */
int parse_rfid_packet(const uint8_t *packet, rfid_fields_t *fields) {
    // 检查起始码（可选）
    if (memcmp(packet, "avery", 5) != 0) {
        ESP_LOGE("PARSE", "起始码错误");
        return 0;
    }

    // 提取各字段（均为3字节ASCII）
    memcpy(fields->version, packet + 6, 3);
    fields->version[3] = '\0';
    memcpy(fields->type, packet + 10, 3);
    fields->type[3] = '\0';
    memcpy(fields->role, packet + 14, 3);
    fields->role[3] = '\0';
    memcpy(fields->timbre, packet + 18, 3);
    fields->timbre[3] = '\0';
    memcpy(fields->reserve, packet + 22, 3);
    fields->reserve[3] = '\0';
    memcpy(fields->crc, packet + 26, 4);

    char *endptr;
    uint16_t stored_crc = (uint16_t)strtol(fields->crc, &endptr, 16);
    // if (*endptr != '\0') return 0;
    // return 1;

    
    uint16_t calc_crc = crc16_ccitt(packet, 25); // 计算前25字节的CRC

    ESP_LOGI("PARSE", "版本: %s", fields->version);
    ESP_LOGI("PARSE", "类型: %s", fields->type);
    ESP_LOGI("PARSE", "角色: %s", fields->role);
    ESP_LOGI("PARSE", "音色: %s", fields->timbre);
    ESP_LOGI("PARSE", "备用: %s", fields->reserve);
    ESP_LOGI("PARSE", "CRC: 存储 0x%04X, 计算 0x%04X", stored_crc, calc_crc);

    if (stored_crc != calc_crc) {
        ESP_LOGE("PARSE", "CRC 校验失败");
        return 0;
    }

    ESP_LOGI("PARSE", "数据包有效");
    return 1;
}



/**
 * 在用户内存中查找 "avery" 并解析数据包
 * @param user_mem  用户内存数据（从页0x04开始）
 * @param mem_len   数据长度
 * @param fields    输出解析后的字段
 * @return 1成功，0失败
 */
int find_and_parse_rfid_data(const uint8_t *user_mem, uint16_t mem_len, rfid_fields_t *fields) {
    const uint8_t header[] = {'a','v','e','r','y'};
    for (uint16_t i = 0; i <= mem_len - 22; i++) {  // 至少22字节才能包含完整数据包
        if (memcmp(user_mem + i, header, 5) == 0) {
            // 找到起始码，尝试解析
            return parse_rfid_packet(user_mem + i, fields);
        }
    }
    ESP_LOGE("PARSE", "未找到起始码 'avery'");
    return 0;
}