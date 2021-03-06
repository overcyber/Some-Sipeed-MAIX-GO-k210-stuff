/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "sleep.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "i2s.h"
#include "sysctl.h"
#include "fpioa.h"
#include "uarths.h"
#include "dmac.h"
#include <math.h>
#include "fft.h"
#include "Adafruit_GFX.h"
#include "my_st7789.h"

#define LCD_X_MAX 240
#define LCD_Y_MAX 320
my_ST7789 tft = my_ST7789(LCD_X_MAX, LCD_Y_MAX);


uint32_t rx_buf[1024];
uint32_t g_index;
uint32_t g_tx_len;

#define FRAME_LEN           512
#define FFT_N               512U
#define FFT_FORWARD_SHIFT   0x0U
#define SAMPLE_RATE         38640


uint32_t g_lcd_gram[38400] __attribute__((aligned(64)));

int16_t  real[FFT_N];
int16_t  imag[FFT_N];
float    hard_power[FFT_N];
uint64_t fft_out_data[FFT_N / 2];
uint64_t buffer_input[FFT_N];
uint64_t buffer_output[FFT_N];

void update_image( float* hard_power, uint32_t* pImage, uint32_t color, uint32_t bkg_color);

int main(void)
{
    int i;
    complex_hard_t data_hard[FFT_N] = {0};
    fft_data_t *output_data;
    fft_data_t *input_data;

    sysctl_pll_set_freq(SYSCTL_PLL0, 480000000UL); // UART, CPU
    sysctl_pll_set_freq(SYSCTL_PLL1, 160000000UL); // ?
    sysctl_pll_set_freq(SYSCTL_PLL2, 45158400UL);  // ?

    uarths_init();

    // on board microphone
    fpioa_set_function(20, FUNC_I2S0_IN_D0);
    fpioa_set_function(19, FUNC_I2S0_WS);
    fpioa_set_function(18, FUNC_I2S0_SCLK);

    tft.init();
    tft.lcd_set_direction(DIR_XY_LRDU);

    // channel 0 set to receive
    i2s_init(I2S_DEVICE_0, I2S_RECEIVER, 0x3);

    i2s_rx_channel_config(I2S_DEVICE_0, I2S_CHANNEL_0, RESOLUTION_16_BIT, SCLK_CYCLES_32, TRIGGER_LEVEL_4, STANDARD_MODE);

    while (1)
    {
        // receive the data from the microphone
        i2s_receive_data_dma(I2S_DEVICE_0, &rx_buf[0], FRAME_LEN * 2, DMAC_CHANNEL3);

        // place the data in a specific format for the fft operation
        for ( i = 0; i < FFT_N / 2; ++i)
        {
            input_data = (fft_data_t *)&buffer_input[i];
            input_data->R1 = rx_buf[2*i];   // data_hard[2 * i].real;
            input_data->I1 = 0;             // data_hard[2 * i].imag;
            input_data->R2 = rx_buf[2*i+1]; // data_hard[2 * i + 1].real;
            input_data->I2 = 0;             // data_hard[2 * i + 1].imag;
        }

        // perform the fft operation
        fft_complex_uint16_dma(DMAC_CHANNEL0, DMAC_CHANNEL1, FFT_FORWARD_SHIFT, FFT_DIR_FORWARD, buffer_input, FFT_N, buffer_output);

        // place the output in a specific format for further calculations
        for ( i = 0; i < FFT_N / 2; i++)
        {
            output_data = (fft_data_t*)&buffer_output[i];
            data_hard[2 * i].imag = output_data->I1 ;
            data_hard[2 * i].real = output_data->R1 ;
            data_hard[2 * i + 1].imag = output_data->I2 ;
            data_hard[2 * i + 1].real = output_data->R2 ;
        }

        // convert data to power as a function of frequency. 
        // Note: the log has to be multiplied by 20 to get the power
        for (i = 0; i < FFT_N; i++)
        {   // calculate the size of the imaginary numbers
            hard_power[i] = sqrt(data_hard[i].real * data_hard[i].real + data_hard[i].imag * data_hard[i].imag);
            //Convert the size to dBFS
            hard_power[i] = 20*log(2*hard_power[i]/FFT_N);
        }

        // draw the graph in memory. 
        update_image(hard_power, g_lcd_gram, RED, BLACK);
        // draw the graph on-screen
        tft.drawPicture(0, 0, LCD_X_MAX, LCD_Y_MAX, g_lcd_gram);
    }
    return 0;
}

#define NUM_FREQ 53

void update_image( float* hard_power, uint32_t* pImage, uint32_t color, uint32_t bkg_color )
{
    uint32_t bcolor= (bkg_color << 16) | bkg_color; // background color
    uint32_t fcolor= (color << 16) | color;         // color of bars
    uint32_t lcolor= (BLUE << 16) | BLUE;           // color of tickmarks

    int h[NUM_FREQ+2];
    int x;
    int i;

    // 140 is the max range dBFS
    // 120 is max of graph
    // 120 / 140 = 0.8571
    for(int i=0; i<NUM_FREQ+2; ++i)
    {
        h[i]=0.8571*hard_power[i];
        if( h[i]>120) h[i] = 120;
        else if( h[i]<0) h[i] = 0;
    }

    // make the bars 4 pixels wide, add 2 empty pixels -> 6 pixels * 53 = 318, the screen is 320 wide
    for( i=0; i<NUM_FREQ; ++i)
    {
        x=i*3;
        for( int y=0; y<120; ++y)
        {
            if( y<h[i+2] )
            {
                pImage[y+x*240]=fcolor;             // 1st and 2nd pixel of bar
                pImage[y+120+x*240]=fcolor;         

                pImage[y+(x+1)*240]=fcolor;         // 3rd and 4th pixel of bar
                pImage[y+120+(x+1)*240]=fcolor;     

                pImage[y+(x+2)*240]=bcolor;         // 5th and 6th pixel of bar
                pImage[y+120+(x+2)*240]=bcolor;
            }
            else // fill all pixels above the graph with background
            {
                pImage[y+x*240]=bcolor;
                pImage[y+120+x*240]=bcolor;
                pImage[y+(x+1)*240]=bcolor;
                pImage[y+120+(x+1)*240]=bcolor;
                pImage[y+(x+2)*240]=bcolor;
                pImage[y+120+(x+2)*240]=bcolor;
            }
        }
    }


    // draws 'tickmark' lines at 1kHz, ... 8kHz (approximately)
   for (i=0; i<8; i++) {
        x = 13 + i*17.5;
        for( int y=0; y<120; ++y) {
            pImage[y+x*240] = lcolor;
        }
    }
}
