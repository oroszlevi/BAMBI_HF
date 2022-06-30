#include <segmentlcd_individual.h>
#include "em_device.h"
#include "em_chip.h"

#include "segmentlcd.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_timer.h"
#include "em_usart.h"

#define TOP 2735 // Compare konstans Timer interrupthoz
#define DUCK_TICK	5	// duck tick: ~14MHz -> 1024 prescaler (lásd timer init) ~> 5/sec

// A pontszámlálóhoz és a játékhoz tartozó kijelzõrész
SegmentLCD_UpperCharSegments_TypeDef score[SEGMENT_LCD_NUM_OF_UPPER_CHARS];
SegmentLCD_LowerCharSegments_TypeDef playfield[SEGMENT_LCD_NUM_OF_LOWER_CHARS];

// Játékelemek
typedef struct{
  const uint16_t sprite;	//bitfiled, a szegmensek grafikus tartalma bitenként kódolva
  uint8_t place: 3;				//melyik digiten helyezkedik el a karakter
  uint8_t visible: 1;			//látható-e
}entity;

volatile entity hunter = {.sprite = 0x0008, .place = 0, .visible = 1};	//vadász (játékos)
volatile entity duck =   {.sprite = 0x0001, .place = 0, .visible = 0};	//kacsa (ezt kell lelőni)
volatile entity shotL =  {.sprite = 0x1000, .place = 0, .visible = 0};	//lövedék a vadászhoz közelebbb
volatile entity shotH =  {.sprite = 0x0100, .place = 0, .visible = 0};	//lövedék a vadásztól távolabb

volatile bool setup = true;				// szintválasztás menüben vagyunk-e (setup), vagy játszunk épp
volatile uint8_t difficulty = 3;	// nehézségi szint
volatile uint8_t duck_wait = 0;		// kacsa egyhelyben állásához

volatile uint8_t villogas_szam = 0;			// ennyit villog majd a kacsa
volatile uint32_t random = 0xAA5DF0C9;	// "véletlenszám"-generátor seed

// Pontszámláló
volatile int total = 1;		// összes próbálkozás
volatile int hits = 0;		// találat

// Animáció lépései (lásd: switch(animation) a Timer interrupton belül - állapotgép)
typedef enum{lohetsz, loves_also, loves_felso, talalt, villogas}animation_phase;
volatile animation_phase animation = lohetsz;

// Kacsa mozgatása (inline, mert interruptból kéne hívni)
static inline void next_duck (void){
	duck.place = random % 7;
	total++;
	duck_wait = 0;
}

// Pontszám kijelzése
void count(){
	const uint8_t numbers[10] = {
			0x3F,	// 0
			0x06,	// 1
			0x5B,	// 2
			0x4F,	// 3
			0x66,	// 4
			0x6D,	// 5
			0x7D,	// 6
			0x07,	// 7
			0x7F,	// 8
			0x6F	// 9
	};

	for(int i=0; i<SEGMENT_LCD_NUM_OF_UPPER_CHARS; i++){	//clear playfield (only bitmap, not being displayed immediately)
		score[i].raw = 0x0000;
	}

	score[0].raw = numbers[hits % 10];			//számjegyek kijelzése
	score[1].raw = numbers[hits / 10];			//  100-nál hülyeség a felsõ helyiértéken, túlindexeli a numbers tömböt
	score[2].raw = numbers[total % 10];			//  (a végsõ játékban úgyis csak 25-ig kell tudni kijelezni...)
	score[3].raw = numbers[total / 10];

	SegmentLCD_Symbol(LCD_SYMBOL_COL10, 1); //kettõspont a felsõ számok közé
	SegmentLCD_UpperSegments(score);
}

// Játékelemek kirajzolása
void drawEntities(void){
	for(int i=0; i<SEGMENT_LCD_NUM_OF_LOWER_CHARS; i++){	//clear playfield (only bitmap, not being displayed immediately)
		playfield[i].raw = 0x0000;
	}

	//karaktereket egymásra rajzolja
	if(duck.visible){
		playfield[duck.place].raw |= duck.sprite;
	}

	if(hunter.visible){
		playfield[hunter.place].raw |= hunter.sprite;
	}

	if(shotL.visible){
		playfield[shotL.place].raw |= shotL.sprite;
	}

	if(shotH.visible){
		playfield[shotH.place].raw |= shotH.sprite;
	}

	SegmentLCD_LowerSegments(playfield);
}

// Start menü, nehézségi szint beállítása
void setDifficulty(void){
	char str[8] = "LEVEL X";
	str[6] = (char)difficulty + '0';	// nem sprintf, mert az overkill ide
	SegmentLCD_Write(str);
}

// TIMER0 interrupt a játék frissítéséhez (tick)
void TIMER0_IRQHandler(void)
{
	// Clear flag for TIMER0 overflow interrupt
	TIMER_IntClear(TIMER0, TIMER_IF_OF);

	// Pszeudo-véletlenszám generálás, lehetőleg minél gyakrabban (Xorshift algoritmust alkalmazva)
	random ^= random << 13;
	random ^= random >> 17;
	random ^= random << 5;

	// Animálás állapotgépe
	switch (animation){
	  case loves_also:			//lövedék megjelenik
			shotH.visible = 0;
			shotL.visible = 1;
			animation = loves_felso;
	  	break;
	  case loves_felso:			//lövedék feljebb megy
			shotH.visible = 1;
			shotL.visible = 0;
			animation = talalt;
	  	break;
	  case talalt:					//lövedék eltűnik (nem tudjuk még, hogy talált-e)
	  	shotH.visible = 0;
	  	if(duck.place == shotH.place){	//találatott kapott
	  		villogas_szam=6;
	  		hits++;
	  		animation = villogas;
	  	}else{
	  		animation = lohetsz;					//nem, újra lehet lőni
	  	}
	  	break;
	  case villogas:				// villogás jelzi a találatot
	  	duck.visible = ~duck.visible;
	  	villogas_szam--;
			if(villogas_szam==0){
				next_duck();
				animation = lohetsz;
			}
	  	break;
	  default:							//ha nem volt lövés-kezdeményezés
	  	break;
	}

	// Ne kerüljön arrébb a kacsa, amíg villog
	if(animation!=villogas){
		duck_wait++;
		//nehézségi szinttől függően más-más ideig várunk a kacsa elpakolásáig
		if(duck_wait==(6-difficulty) * DUCK_TICK){
			next_duck();
		}
	}
}

void startTimer(void){
  /* Enable overflow interrupt */
  TIMER_IntEnable(TIMER0, TIMER_IF_OF);
}

void stopTimer(void){
  /* Enable overflow interrupt */
  TIMER_IntDisable(TIMER0, TIMER_IF_OF);
}

// UART interrupt
void UART0_RX_IRQHandler(void) {
	uint8_t ch;
	ch = USART_RxDataGet(UART0);
	if(setup){ 							//nehézségi mód állítás, limitálással
			if (ch == '+'){
				difficulty++;
				if(difficulty>5){
					difficulty = 5;
				}
			}else if (ch == '-'){
				difficulty--;
				if(difficulty<1) {
					difficulty = 1;
				}
			}
			// indul a játék
			else if (ch == 's'){
				setup = false;
			}
		}else{
			if (animation==lohetsz && ch == 'a') { //vadász lő
				  shotL.place=shotH.place=hunter.place;
				  animation = loves_also;
			  }
			  else if (ch == 'j') { //vadász jobbra lép, pálya széléig tud menni
				  hunter.place++;
				  if(hunter.place>SEGMENT_LCD_NUM_OF_LOWER_CHARS-1){
				  	hunter.place = SEGMENT_LCD_NUM_OF_LOWER_CHARS-1;
				  }
			 }else if (ch == 'b') { //vadász balra lép, pálya széléig tud menni
				  hunter.place--;
				  if(hunter.place==7){
				  	hunter.place = 0;
				  }
			 }else if (ch == 'r') {	//kilépés a start menübe, új játékhoz
				 setup = true;
			 }
		}

	USART_IntClear(UART0, USART_IF_RXDATAV); // kell, a végére, ha nem törli magától a flaget - DE TÖRLI XD TODO
}

// UART inicializálása
void UARTSetup(void){

	//UART GPIO setup
	CMU_ClockEnable(cmuClock_GPIO, true);
	GPIO_PinModeSet(gpioPortF, 7, gpioModePushPull, 1);

	GPIO_PinModeSet(gpioPortE, 1, gpioModeInput, 0);

	CMU_ClockEnable(cmuClock_UART0, true);
	USART_InitAsync_TypeDef UART0_init = USART_INITASYNC_DEFAULT;
	UART0_init.enable 			= usartEnableRx;

	USART_InitAsync(UART0, &UART0_init);

	//kivezetés az eredeti helyett máshova
	UART0->ROUTE |= USART_ROUTE_LOCATION_LOC1; //(1) << 8;
	UART0->ROUTE |= (USART_ROUTE_RXPEN);

	USART_IntEnable(UART0, USART_IF_RXDATAV);
	NVIC_EnableIRQ(UART0_RX_IRQn);
}

//TIMER0 setup tick-hez
void timerSetup(void){
	/* Enable clock for TIMER0 module */
  CMU_ClockEnable(cmuClock_TIMER0, true);

  /* Select TIMER0 parameters */
	TIMER_Init_TypeDef timerInit =
	{
		.enable     = true,
		.debugRun   = true,
		.prescale   = timerPrescale1024,
		.clkSel     = timerClkSelHFPerClk,
		.fallAction = timerInputActionNone,
		.riseAction = timerInputActionNone,
		.mode       = timerModeUp,
		.dmaClrAct  = false,
		.quadModeX4 = false,
		.oneShot    = false,
		.sync       = false,
	};

	/* Enable TIMER0 interrupt vector in NVIC */
	NVIC_EnableIRQ(TIMER0_IRQn);

	/* Set TIMER Top value */
	TIMER_TopSet(TIMER0, TOP);

	/* Configure TIMER */
	TIMER_Init(TIMER0, &timerInit);
}

int main(void)
{
	//inicializáljuk az eszközt
  SegmentLCD_Init(false);
  timerSetup();
  UARTSetup();

  while(1){
  		int total_copy = 1;		//interrupt miatt kell

  		total = 1;						//előző játék közben nyomott 'r' miatt kell a változók újrainicializálása
  		hits = 0;
  		duck_wait = 0;
  		animation = lohetsz;
  		villogas_szam = 0;

  		hunter.place=3;
  		duck.visible=1;
  		shotL.visible = 0;
  		shotH.visible = 0;

  		SegmentLCD_AllOff();	// képernyőtörlés
  		while(setup==true){		// beállítjuk a nehézségi szintet
  			setDifficulty();
  		}

  		startTimer();					// indul a játék

  		// csak gyors számolások, és kirajzolás történnekk benne
  		while(total_copy<=25 && !setup){
  			drawEntities();
  			count();
  			total_copy = total;
  		};

  		stopTimer();					//a játék leállítása

  		SegmentLCD_AlphaNumberOff();
  		SegmentLCD_Write("GEMOVER");
  		LCD_SegmentSet(4,13,false); //'G' előállítása 6-os helyett

  		while(setup == false);	// várunk az új játékra
    }
}
