/*

PROJECT CLAPTRAP ARDUINO CODE

Author: Gil Rupert Licu
Last update: July 2016

Current goal: robot balancing

*/


// ================================================================
// ===              LIBRARIES AND VARIABLE DEFINES              ===
// ================================================================

#include "I2Cdev.h"

#include "MPU6050_6Axis_MotionApps20.h"
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
	#include "Wire.h"
#endif

MPU6050 mpu;

#define OUTPUT_READABLE_YAWPITCHROLL

#define INTERRUPT_PIN 2  // use pin 2 on Arduino Uno & most boards
#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
#define PWM_A 5
#define PWM_B 6
bool blinkState = false;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector
int16_t gyro[3];        // [x, y, z]            gyro vector
int16_t accel[3];        // [x, y, z]            accel vector

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };

float out;
float angle;
float anglespeed;
float angleerror;
float expectedangle;
float initialangle;
float anglelimithigh;
float anglelimitlow;
float lowoutthreshold;
float outprev[2];
float kp;
float ki;
float kd;
float dampout;
float realangle;
float dampgyro[3];
float dgyro;
bool standstate = true; // true if just standing
bool standwobble = false; // true-forward, false-backward
float kicum;
float counter;
float absangle;

// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
	mpuInterrupt = true;
}

// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {
	// join I2C bus (I2Cdev library doesn't do this automatically)
	#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
		Wire.begin();
		Wire.setClock(400000); // 400kHz I2C clock. Comment this line if having compilation difficulties
	#elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
		Fastwire::setup(400, true);
	#endif

	// initialize serial communication
	// (115200 chosen because it is required for Teapot Demo output, but it's
	// really up to you depending on your project)
	Serial.begin(115200);
	while (!Serial); // wait for Leonardo enumeration, others continue immediately

	// NOTE: 8MHz or slower host processors, like the Teensy @ 3.3v or Ardunio
	// Pro Mini running at 3.3v, cannot handle this baud rate reliably due to
	// the baud timing being too misaligned with processor ticks. You must use
	// 38400 or slower in these cases, or use some kind of external separate
	// crystal solution for the UART timer.

	// initialize device
	Serial.println(F("Initializing I2C devices..."));
	mpu.initialize();
	pinMode(INTERRUPT_PIN, INPUT);

	// verify connection
	Serial.println(F("Testing device connections..."));
	Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

	// wait for ready
	//Serial.println(F("\nSend any character to begin DMP programming and demo: "));
	//while (Serial.available() && Serial.read()); // empty buffer
	//while (!Serial.available());                 // wait for data
	//while (Serial.available() && Serial.read()); // empty buffer again

	// load and configure the DMP
	Serial.println(F("Initializing DMP..."));
	devStatus = mpu.dmpInitialize();

	// supply your own gyro offsets here, scaled for min sensitivity
	//mpu.setXGyroOffset(220);
	//mpu.setYGyroOffset(76);
	//mpu.setZGyroOffset(-85);
	//mpu.setZAccelOffset(1788); // 1688 factory default for my test chip
	mpu.setXGyroOffset(77);
	mpu.setYGyroOffset(13);
	mpu.setZGyroOffset(91);
	mpu.setXAccelOffset(-694);
	mpu.setYAccelOffset(139);
	mpu.setZAccelOffset(1718);

	// make sure it worked (returns 0 if so)
	if (devStatus == 0) {
		// turn on the DMP, now that it's ready
		Serial.println(F("Enabling DMP..."));
		mpu.setDMPEnabled(true);

		// enable Arduino interrupt detection
		Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
		attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), dmpDataReady, RISING);
		mpuIntStatus = mpu.getIntStatus();

		// set our DMP Ready flag so the main loop() function knows it's okay to use it
		Serial.println(F("DMP ready! Waiting for first interrupt..."));
		dmpReady = true;

		// get expected DMP packet size for later comparison
		packetSize = mpu.dmpGetFIFOPacketSize();
	} else {
		// ERROR!
		// 1 = initial memory load failed
		// 2 = DMP configuration updates failed
		// (if it's going to break, usually the code will be 1)
		Serial.print(F("DMP Initialization failed (code "));
		Serial.print(devStatus);
		Serial.println(F(")"));
	}

	// configure LED for output
	pinMode(LED_PIN, OUTPUT);
	pinMode(PWM_A, OUTPUT);
	pinMode(PWM_B, OUTPUT);

	digitalWrite(PWM_A, LOW);
	digitalWrite(PWM_B, LOW);

	initialangle = 47.62;
	expectedangle = initialangle;
	anglelimithigh = 20;
	anglelimitlow = -20;
	lowoutthreshold = 30;
	outprev[0] = 0;
	outprev[1] = 0;
	dampout = 0;
	realangle = 0;
	dampgyro[0] = 0;
	dampgyro[1] = 0;
	dampgyro[2] = 0;
	dgyro = 0;
	kicum = 0;
	counter = 1;
	absangle = 0;

	delay(7000);
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
	
	// if programming failed, don't try to do anything
	if (!dmpReady) return;

	// wait for MPU interrupt or extra packet(s) available
	while (!mpuInterrupt && fifoCount < packetSize) {
		// other program behavior stuff here
		// .
		// .
		// .
		// if you are really paranoid you can frequently test in between other
		// stuff to see if mpuInterrupt is true, and if so, "break;" from the
		// while() loop to immediately process the MPU data
		// .
		// .
		// .
	}

	// reset interrupt flag and get INT_STATUS byte
	mpuInterrupt = false;
	mpuIntStatus = mpu.getIntStatus();

	// get current FIFO count
	fifoCount = mpu.getFIFOCount();

	// check for overflow (this should never happen unless our code is too inefficient)
	if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
		// reset so we can continue cleanly
		mpu.resetFIFO();
		//Serial.println(F("FIFO overflow!"));
	// otherwise, check for DMP data ready interrupt (this should happen frequently)
	} else if (mpuIntStatus & 0x02) {
		// wait for correct available data length, should be a VERY short wait
		while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

		// read a packet from FIFO
		mpu.getFIFOBytes(fifoBuffer, packetSize);

		// track FIFO count here in case there is > 1 packet available
		// (this lets us immediately read more without waiting for an interrupt)
		fifoCount -= packetSize;


		mpu.dmpGetQuaternion(&q, fifoBuffer);
		mpu.dmpGetGravity(&gravity, &q);
		mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
		mpu.dmpGetGyro(gyro, fifoBuffer);
		mpu.dmpGetAccel(accel, fifoBuffer);


		/*
		Serial.print("accel\t");
		Serial.print((double)accel[0]);
		Serial.print("\t");
		Serial.print((double)accel[1]);
		Serial.print("\t");
		Serial.println((double)accel[2]);
		*/

		/*
		Serial.print("gyro\t");
		Serial.print((double)gyro[0]);
		Serial.print("\t");
		Serial.print((double)gyro[1]);
		Serial.print("\t");
		Serial.println((double)gyro[2]);
		*/
		  
		/*
		Serial.print("ypr\t");
		Serial.print(ypr[0] * 180/M_PI);
		Serial.print("\t");
		Serial.print(ypr[1] * 180/M_PI);
		Serial.print("\t");
		Serial.println(ypr[2] * 180/M_PI);
		*/
		
		dampgyro[2] = dampgyro[1];
		dampgyro[1] = dampgyro[0];
		dampgyro[0] = (float) gyro[1];
		anglespeed = (dampgyro[0] + dampgyro[1] + dampgyro[2])/3;
		
		
		realangle = ypr[1]*180/M_PI - initialangle;
		//angle = realangle + constrain( (dampout*0.05) , (float)-3.2, (float)3.2);
		//angle = realangle + pow(dampout*2,3)*10;
		angle = realangle;
		
		/*
		if (standwobble){
			angle = angle + 1.7;
		}else{
			angle = angle - 1.7;
		}
		
		if ( (realangle-0.3)>0 && outprev[0]>0 ) {
			standwobble = true;
		} else if ( (realangle+0.3)<0 && outprev[0]<0 ) {
			standwobble = false;
		}*/
		
		/*
		Serial.print("standwobble=");
		Serial.print(standwobble);
		Serial.print("  realangle=");
		Serial.print(realangle);
		Serial.print("  out=");
		Serial.println(out);
		*/
		
		/*
		Serial.print("dampout=");
		Serial.print(dampout);
		Serial.print("  realangle=");
		Serial.print(realangle);
		Serial.print("  angle=");
		Serial.println(angle);
		*/
		
		//anglespeed = ((float)gyro[1]) + 0.5;
		//anglespeed = (float)0;
		//anglespeed = ( dgyro ) + 0.5;
		angleerror = angle - expectedangle;
		
		kicum = (angle + kicum*0.80);
		
		absangle = abs(angle);
		
		kp = angle*absangle*7 + angleerror*190;
		ki = min( (kicum/counter) , 0.3 )*400;
		kd = -anglespeed*( min(4/absangle, (float)500.0) )*( absangle )*0.1;
		out = kp + ki + kd;
		//out = outprev1*0.6 + out*0.4;
		out = out*0.7 + outprev[0]*0.2 + outprev[1]*0.1;
		
		if (realangle<anglelimitlow || realangle>anglelimithigh){
			out = 0;
			dampout = 0;
		}
		if((out>0 && out<lowoutthreshold) || (out<0 && out>(-lowoutthreshold))){
			out = 0;
		}
		
		counter++;
		if (counter>4096){
			kicum = (kicum/counter)*10;
			counter = 10;
		}
		
		//dampout = out*0.00003 + dampout*1.00001;
		//dampout = out*0.01 + dampout*0.88;

		expectedangle = angle*0.80;
		outprev[1] = outprev[0];
		outprev[0] = out;
		
		/*
		Serial.print("dampout=");
		Serial.print(dampout);
		Serial.print("  out=");
		Serial.println(out);
		*/
		
		/*
		Serial.print("  out=");
		Serial.print(out);
		Serial.print("  kp=");
		Serial.print(kp);
		Serial.print("  ki=");
		Serial.print(ki);
		Serial.print("  kd=");
		Serial.println(kd);
		*/
		

		//out=0;
		if (out>0){
			digitalWrite(PWM_A, LOW);
			analogWrite(PWM_B, constrain(out,0,255));
		}else{
			digitalWrite(PWM_B, LOW);
			analogWrite(PWM_A, constrain(-out,0,255));
		}


		// blink LED to indicate activity
		//blinkState = !blinkState;
		//digitalWrite(LED_PIN, blinkState);
		//digitalWrite(LED_PIN, false);
	}
}
