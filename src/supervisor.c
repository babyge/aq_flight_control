/*
    This file is part of AutoQuad.

    AutoQuad is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AutoQuad is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with AutoQuad.  If not, see <http://www.gnu.org/licenses/>.

    Copyright � 2011-2014  Bill Nesbitt
    Copyright 2013-2016 Maxim Paperno
*/

#include "aq.h"
#include "config.h"
#include "supervisor.h"
#include "aq_mavlink.h"
#include "comm.h"
#include "digital.h"
#include "radio.h"
#include "rc.h"
#include "nav.h"
#include "analog.h"
#include "aq_timer.h"
#include "util.h"
#include "nav_ukf.h"
#include "gps.h"
#include "d_imu.h"
#include "motors.h"
#include "calib.h"
#include "run.h"
#include "canSensors.h"
#ifdef USE_SIGNALING
#include "signaling.h"
#endif
#include <stdlib.h>
#include <string.h>

supervisorStruct_t supervisorData __attribute__((section(".ccm")));

OS_STK *supervisorTaskStack;

void supervisorLEDsOn(void) {
#ifdef SUPERVISOR_DEBUG_PORT
    digitalHi(supervisorData.debugLed);
#endif
    digitalHi(supervisorData.readyLed);
    digitalHi(supervisorData.gpsLed);
}

void supervisorLEDsOff(void) {
#ifdef SUPERVISOR_DEBUG_PORT
    digitalLo(supervisorData.debugLed);
#endif
    digitalLo(supervisorData.readyLed);
    digitalLo(supervisorData.gpsLed);
}

static void supervisorSetSystemStatus(void) {

    // set the base status (these are mutually exclusive)
    if (supervisorData.state & STATE_FLYING)
	supervisorData.systemStatus = SPVR_AQ_STATUS_ACTIVE;
    else if (supervisorData.state & STATE_ARMED)
	supervisorData.systemStatus = SPVR_AQ_STATUS_STANDBY;
    else if (supervisorData.state & STATE_CALIBRATION)
	supervisorData.systemStatus =  SPVR_AQ_STATUS_CALIBRATING;
    else
	supervisorData.systemStatus = SPVR_AQ_STATUS_READY;

    // rest of the status flags can be cumulative

    if ((supervisorData.state & STATE_RADIO_LOSS1))
	supervisorData.systemStatus |= SPVR_AQ_STATUS_NO_RC;

    if ((supervisorData.state & STATE_RADIO_LOSS2))
	supervisorData.systemStatus |= SPVR_AQ_STATUS_FAILSAFE;

    if (supervisorData.state & STATE_LOW_BATTERY2)
	supervisorData.systemStatus |=  SPVR_AQ_STATUS_FUEL_CRITICAL;
    else if (supervisorData.state & STATE_LOW_BATTERY1)
	supervisorData.systemStatus |=  SPVR_AQ_STATUS_FUEL_LOW;

    if (navData.ceilingAlt) {
	supervisorData.systemStatus |= SPVR_AQ_STATUS_CEILING;
	if (navData.setCeilingReached)
	    supervisorData.systemStatus |= SPVR_AQ_STATUS_CEILING_REACHED;
    }

    if (navData.mode <= NAV_STATUS_MANUAL)
	return;

    switch(navData.mode) {
	case NAV_STATUS_ALTHOLD:
	    supervisorData.systemStatus |= SPVR_AQ_STATUS_ALTHOLD;
	    break;

	case NAV_STATUS_POSHOLD:
	    supervisorData.systemStatus |= SPVR_AQ_STATUS_ALTHOLD | SPVR_AQ_STATUS_POSHOLD;
	    break;

	case NAV_STATUS_DVH:
	    supervisorData.systemStatus |= SPVR_AQ_STATUS_ALTHOLD | SPVR_AQ_STATUS_POSHOLD | SPVR_AQ_STATUS_DVH;
	    break;

	case NAV_STATUS_MISSION:
	    supervisorData.systemStatus |= SPVR_AQ_STATUS_MISSION;
	    break;
    }

    if (navData.verticalOverride)
	supervisorData.systemStatus |= SPVR_AQ_STATUS_DAO;

    if (navData.headFreeMode == NAV_HEADFREE_DYNAMIC)
	supervisorData.systemStatus |= SPVR_AQ_STATUS_HF_DYNAMIC;
    else if (navData.headFreeMode == NAV_HEADFREE_LOCKED)
	supervisorData.systemStatus |= SPVR_AQ_STATUS_HF_LOCKED;

    // FIXME: we need a better indicator of whether we're actually RingTH
    if (rcIsSwitchActive(NAV_CTRL_HOM_GO))
	supervisorData.systemStatus |= SPVR_AQ_STATUS_RTH;

}

void supervisorArm(void) {
    uint8_t rcStat = rcCheckValidController();
    if (rcStat) {
	AQ_NOTICE("Error: Can't arm due to RC error(s):\n");
	rcReportAllErrors(rcStat);
    }
    else if (rcIsSwitchActive(NAV_CTRL_AH) || rcIsSwitchActive(NAV_CTRL_PH) || rcIsSwitchActive(NAV_CTRL_MISN))
	AQ_NOTICE("Error: Can't arm, not in manual flight mode.\n");
    else if (rcIsSwitchActive(NAV_CTRL_HOM_SET) || rcIsSwitchActive(NAV_CTRL_HOM_GO))
	AQ_NOTICE("Error: Can't arm, home command active.\n");
    else if (rcIsSwitchActive(NAV_CTRL_HF_SET) || rcIsSwitchActive(NAV_CTRL_HF_LOCK))
	AQ_NOTICE("Error: Can't arm, heading-free mode active.\n");
    else if (motorsArm()) {
	supervisorData.state = STATE_ARMED | (supervisorData.state & (STATE_LOW_BATTERY1 | STATE_LOW_BATTERY2));
	AQ_NOTICE("Armed\n");
#ifdef USE_SIGNALING
	signalingOnetimeEvent(SIG_EVENT_OT_ARMING);
#endif
    }
    else {
	motorsDisarm();
	AQ_NOTICE("Error: Arm motors failed - disarmed.\n");
    }
}

void supervisorDisarm(void) {
    motorsDisarm();
    calibDeinit();
    supervisorLEDsOff();
    supervisorData.state = STATE_DISARMED | (supervisorData.state & (STATE_LOW_BATTERY1 | STATE_LOW_BATTERY2));
    AQ_NOTICE("Disarmed\n");
#ifdef USE_SIGNALING
    signalingOnetimeEvent(SIG_EVENT_OT_DISARMING);
#endif
}

void supervisorCalibrate(void) {
    supervisorData.state = STATE_CALIBRATION;
    AQ_NOTICE("Starting MAG calibration mode.\n");
    calibInit();
}

void supervisorTare(void) {
    supervisorLEDsOn();
    dIMUTare();
    AQ_NOTICE("Level calibration complete.\n");
    supervisorLEDsOff();
}

void supervisorTaskCode(void *unused) {
    unsigned long lastAqCounter = 0;  // used for idle time calc
    uint32_t count = 0;

    AQ_NOTICE("Supervisor task started\n");

    // wait for ADC vIn data
    while (analogData.batCellCount == 0)
	yield(100);

    supervisorData.vInLPF = analogData.vIn;

    if (analogData.extAmp > 0.0f)
	supervisorData.currentSenseValPtr = &analogData.extAmp;
    else if (canSensorsData.values[CAN_SENSORS_PDB_BATA] > 0.0f)
	supervisorData.currentSenseValPtr = &canSensorsData.values[CAN_SENSORS_PDB_BATA];
    else
	supervisorData.aOutLPF = SUPERVISOR_INVALID_AMPSOUT_VALUE;

    if (supervisorData.currentSenseValPtr)
	supervisorData.aOutLPF = *supervisorData.currentSenseValPtr;

    while (1) {
	yield(1000/SUPERVISOR_RATE);

	if (supervisorData.state & STATE_CALIBRATION) {
	    int i;

	    // try to indicate completion percentage
	    i = constrainInt(20*((calibData.percentComplete)/(100.0f/3.0f)), 1, 21);
	    if (i > 20)
		digitalHi(supervisorData.readyLed);
	    else if (!(count % i))
		digitalTogg(supervisorData.readyLed);

#ifdef SUPERVISOR_DEBUG_PORT
	    i = constrainInt(20*((calibData.percentComplete-100.0f/3.0f)/(100.0f/3.0f)), 1, 21);
	    if (i > 20)
		digitalHi(supervisorData.debugLed);
	    else if (!(count % i))
		digitalTogg(supervisorData.debugLed);
#endif //SUPERVISOR_DEBUG_PORT

	    i = constrainInt(20*((calibData.percentComplete-100.0f/3.0f*2.0f)/(100.0f/3.0f)), 1, 21);
	    if (i > 20)
		digitalHi(supervisorData.gpsLed);
	    else if (!(count % i))
		digitalTogg(supervisorData.gpsLed);

	    // user looking to go back to DISARMED mode?
	    if (RADIO_THROT < p[CTRL_MIN_THROT] && RADIO_RUDD < -500) {
		if (!supervisorData.armTime) {
		    supervisorData.armTime = timerMicros();
		}
		else if ((timerMicros() - supervisorData.armTime) > SUPERVISOR_DISARM_TIME) {
		    supervisorDisarm();
		    supervisorData.armTime = 0;
		}
	    }
	    else {
		supervisorData.armTime = 0;
	    }
	}  // end if calibrating
	else if (supervisorData.state & STATE_DISARMED) {

#ifdef SUPERVISOR_DEBUG_PORT
	    // 0.5 Hz blink debug LED if config file could be found on uSD card
	    if (supervisorData.configRead && (!(count % SUPERVISOR_RATE))) {
		// only for first 15s
		if (timerMicros() > 15000000) {
		    supervisorData.configRead = 0;
		    digitalLo(supervisorData.debugLed);
		}
		else {
		    digitalTogg(supervisorData.debugLed);
		}
	    }
#endif // SUPERVISOR_DEBUG_PORT

	    // 1 Hz blink if disarmed, 5 Hz if writing to uSD card
	    if (!(count % ((supervisorData.diskWait) ? SUPERVISOR_RATE/10 : SUPERVISOR_RATE/2)))
		digitalTogg(supervisorData.readyLed);

	    // Attempt to arm if throttle down and yaw full right for 2sec.
	    if (RADIO_VALID && RADIO_THROT < p[CTRL_MIN_THROT] && RADIO_RUDD > +500) {
		if (!supervisorData.armTime) {
		    supervisorData.armTime = timerMicros();
		}
		else if ((timerMicros() - supervisorData.armTime) > SUPERVISOR_DISARM_TIME) {
		    supervisorArm();
		    supervisorData.armTime = 0;
		}
	    }
	    else {
		supervisorData.armTime = 0;
	    }

	    // various functions
	    if (RADIO_VALID && RADIO_THROT < p[CTRL_MIN_THROT] && RADIO_RUDD < -500) {
		if (!supervisorData.stickCmdTimer) {
		    supervisorData.stickCmdTimer = timerMicros();
		} else if ((timerMicros() - supervisorData.stickCmdTimer) > SUPERVISOR_STICK_CMD_TIME) {

#ifdef HAS_DIGITAL_IMU
		    // tare function (lower left)
		    if (RADIO_ROLL < -500 && RADIO_PITCH > +500) {
			supervisorTare();
			supervisorData.stickCmdTimer = 0;
		    }
		    else
#endif // HAS_DIGITAL_IMU
		    // config write (upper right)
		    if (RADIO_ROLL > +500 && RADIO_PITCH < -500) {
			supervisorLEDsOn();
			configSaveParamsToFlash();
#ifdef DIMU_HAVE_EEPROM
			dIMURequestCalibWrite();
#endif
			supervisorLEDsOff();
			supervisorData.stickCmdTimer = 0;
		    }
		    // calibration mode (upper left)
		    else if (RADIO_ROLL < -500 && RADIO_PITCH < -500) {
			supervisorCalibrate();
			supervisorData.stickCmdTimer = 0;
		    }
		    // clear waypoints (lower right with WP Record switch active)
		    else if (RADIO_ROLL > 500 && RADIO_PITCH > 500 && rcIsSwitchActive(NAV_CTRL_WP_REC)) {
			navClearWaypoints();
			supervisorData.stickCmdTimer = 0;
		    }
		} // end stick timer check
	    }
	    // no stick commands detected
	    else
		supervisorData.stickCmdTimer = 0;

	} // end if disarmed
	else if (supervisorData.state & STATE_ARMED) {
	    // Disarm only if in manual mode
	    if (RADIO_THROT < p[CTRL_MIN_THROT] && RADIO_RUDD < -500 && navData.mode == NAV_STATUS_MANUAL) {
		if (!supervisorData.armTime) {
		    supervisorData.armTime = timerMicros();
		}
		else if ((timerMicros() - supervisorData.armTime) > SUPERVISOR_DISARM_TIME) {
		    supervisorDisarm();
		    supervisorData.armTime = 0;
		}
	    }
	    else {
		supervisorData.armTime = 0;
	    }
	}
	// end of calibrating/disarmed/armed mode checks

	// radio loss
	if ((supervisorData.state & STATE_FLYING) && (navData.mode < NAV_STATUS_MISSION || (supervisorData.state & STATE_RADIO_LOSS2))) {
	    // regained?
	    if (RADIO_QUALITY > 1.0f) {
		supervisorData.lastGoodRadioMicros = timerMicros();

		if (supervisorData.state & STATE_RADIO_LOSS1)
		    AQ_NOTICE("Warning: radio signal regained\n");

		navData.spvrModeOverride = 0;
		supervisorData.state &= ~(STATE_RADIO_LOSS1 | STATE_RADIO_LOSS2);
	    }
	    // loss 1
	    else if (!(supervisorData.state & STATE_RADIO_LOSS1) && (timerMicros() - supervisorData.lastGoodRadioMicros) > SUPERVISOR_RADIO_LOSS1) {
		supervisorData.state |= STATE_RADIO_LOSS1;
		AQ_NOTICE("Warning: Radio loss stage 1 detected\n");

		// hold position
		navData.spvrModeOverride = NAV_STATUS_POSHOLD;
		RADIO_PITCH = 0;    // center sticks
		RADIO_ROLL = 0;
		RADIO_RUDD = 0;
		RADIO_THROT = RADIO_MID_THROTTLE;  // center throttle
	    }
	    // loss 2
	    else if (!(supervisorData.state & STATE_RADIO_LOSS2) && (timerMicros() - supervisorData.lastGoodRadioMicros) > SUPERVISOR_RADIO_LOSS2) {
		supervisorData.state |= STATE_RADIO_LOSS2;
		AQ_NOTICE("Warning: Radio loss stage 2! Initiating recovery.\n");

		// only available with GPS
		if (navData.navCapable) {
		    navMission_t *wp;
		    uint8_t wpi = 0;
		    uint8_t fsOption = (uint8_t)p[SPVR_FS_RAD_ST2];

		    navClearWaypoints();
		    wp = navGetWaypoint(wpi++);

		    if (fsOption > SPVR_OPT_FS_RAD_ST2_LAND && navCalcDistance(gpsData.lat, gpsData.lon, navData.homeLeg.targetLat, navData.homeLeg.targetLon) > SUPERVISOR_HOME_POS_DETECT_RADIUS) {
			float targetAltitude;

			// ascend
			if (fsOption == SPVR_OPT_FS_RAD_ST2_ASCEND && ALTITUDE < navData.homeLeg.targetAlt + p[SPVR_FS_ADD_ALT]) {
			    // the home leg's altitude is recorded without pressure offset
			    targetAltitude = navData.homeLeg.targetAlt + p[SPVR_FS_ADD_ALT] + navData.presAltOffset;

			    wp->type = NAV_LEG_GOTO;
			    wp->relativeAlt = 0;
			    wp->targetAlt = targetAltitude;
			    wp->targetLat = gpsData.lat;
			    wp->targetLon = gpsData.lon;
			    wp->targetRadius = SUPERVISOR_HOME_ALT_DETECT_MARGIN;
			    wp->maxHorizSpeed = navData.homeLeg.maxHorizSpeed;
			    wp->maxVertSpeed = navData.homeLeg.maxVertSpeed;
			    wp->poiHeading = navData.homeLeg.poiHeading;
			    wp->loiterTime = 0;
			    wp->poiAltitude = 0.0f;

			    wp = navGetWaypoint(wpi++);
			}
			else {
			    // the greater of our current altitude or home's altitude
			    targetAltitude = ((ALTITUDE > navData.homeLeg.targetAlt) ? ALTITUDE : navData.homeLeg.targetAlt) + navData.presAltOffset;
			}

			// go home with previously determined altitude
			wp->type = NAV_LEG_GOTO;
			wp->relativeAlt = 0;
			wp->targetAlt = targetAltitude;
			wp->targetLat = navData.homeLeg.targetLat;
			wp->targetLon = navData.homeLeg.targetLon;
			wp->targetRadius = SUPERVISOR_HOME_POS_DETECT_RADIUS;
			wp->maxHorizSpeed = navData.homeLeg.maxHorizSpeed;
			wp->maxVertSpeed = navData.homeLeg.maxVertSpeed;
			wp->poiHeading = navData.homeLeg.poiHeading;
			wp->loiterTime = 0;
			wp->poiAltitude = 0.0f;

			wp = navGetWaypoint(wpi++);

			// decend to home
			wp->type = NAV_LEG_HOME;
			wp->targetRadius = SUPERVISOR_HOME_POS_DETECT_RADIUS;
			wp->loiterTime = 0;
			wp->poiAltitude = 0.0f;

			wp = navGetWaypoint(wpi++);
		    }

		    // land
		    wp->type = NAV_LEG_LAND;
		    wp->maxVertSpeed = NAV_DFLT_LND_SPEED;
		    wp->maxHorizSpeed = 0.0f;
		    wp->poiAltitude = 0.0f;

		    // go
		    navData.missionLeg = 0;
		    navData.tempMissionLoaded = 1;
		    navData.spvrModeOverride = NAV_STATUS_MISSION;
		}
		// no GPS, slow decent in PH mode
		else {
		    navData.spvrModeOverride = NAV_STATUS_POSHOLD;
		    RADIO_PITCH = 0;    // center sticks
		    RADIO_ROLL = 0;
		    RADIO_RUDD = 0;
		    RADIO_THROT = RADIO_MID_THROTTLE * 3 / 4;  // 1/4 max decent
		}
	    }
	}
	// end radio loss check

	// calculate idle time
	supervisorData.idlePercent = (counter - lastAqCounter) * minCycles * 100.0f / ((1e6f / SUPERVISOR_RATE) * rccClocks.SYSCLK_Frequency / 1e6f);
	lastAqCounter = counter;

	// smooth vIn readings
	supervisorData.vInLPF += (analogData.vIn - supervisorData.vInLPF) * (0.1f / SUPERVISOR_RATE);

	// smooth current flow readings, if any
	if (supervisorData.currentSenseValPtr)
	    supervisorData.aOutLPF += (*supervisorData.currentSenseValPtr - supervisorData.aOutLPF) * (0.1f / SUPERVISOR_RATE);

	//calculate remaining battery % based on configured low batt stg 2 level -- ASSumes 4.2v/cell maximum
	supervisorData.battRemainingPrct = (supervisorData.vInLPF - p[SPVR_LOW_BAT2] * analogData.batCellCount) / ((4.2f - p[SPVR_LOW_BAT2]) * analogData.batCellCount) * 100;

	// low battery
	if (!(supervisorData.state & STATE_LOW_BATTERY1) && supervisorData.vInLPF < (p[SPVR_LOW_BAT1]*analogData.batCellCount)) {
	    supervisorData.state |= STATE_LOW_BATTERY1;
	    AQ_NOTICE("Warning: Low battery stage 1\n");

	    // TODO: something
	}
	else if (!(supervisorData.state & STATE_LOW_BATTERY2) && supervisorData.vInLPF < (p[SPVR_LOW_BAT2]*analogData.batCellCount)) {
	    supervisorData.state |= STATE_LOW_BATTERY2;
	    AQ_NOTICE("Warning: Low battery stage 2\n");

	    // TODO: something
	}
	// end battery level checks

	supervisorSetSystemStatus();

	if (supervisorData.state & STATE_FLYING) {
	    // count flight time in seconds
	    supervisorData.flightTime += (1.0f / SUPERVISOR_RATE);

	    // rapidly flash Ready LED if we are critically low on power
	    if (supervisorData.state & STATE_LOW_BATTERY2)
		digitalTogg(supervisorData.readyLed);
	}
	else if (supervisorData.state & STATE_ARMED) {
	    digitalHi(supervisorData.readyLed);
	}

#ifdef SUPERVISOR_DEBUG_PORT
	// DEBUG LED to indicate radio RX state
	if (!supervisorData.configRead && RADIO_INITIALIZED && supervisorData.state != STATE_CALIBRATION) {
	    // packet received in the last 50ms?
	    if (RADIO_VALID) {
		digitalHi(supervisorData.debugLed);
	    }
	    else {
		if (RADIO_BINDING)
		    digitalTogg(supervisorData.debugLed);
		else
		    digitalLo(supervisorData.debugLed);
	    }
	}
#endif

	count++;

#ifdef USE_SIGNALING
	signalingEvent();
#endif
    }
}

void supervisorInitComplete(void) {
    supervisorDisarm();
}

void supervisorDiskWait(uint8_t waiting) {
    supervisorData.diskWait = waiting;
}

void supervisorThrottleUp(uint8_t throttle) {
    if (throttle)
	supervisorData.state |= STATE_FLYING;
    else
	supervisorData.state &= ~STATE_FLYING;
}

void supervisorSendDataStart(void) {
#ifdef SUPERVISOR_DEBUG_PORT
    if (!RADIO_VALID)
	digitalTogg(supervisorData.debugLed);
#endif
}

void supervisorSendDataStop(void) {
#ifdef SUPERVISOR_DEBUG_PORT
    if (!RADIO_VALID)
	digitalTogg(supervisorData.debugLed);
#endif
}

void supervisorConfigRead(void) {
    supervisorData.configRead = 1;
#ifdef SUPERVISOR_DEBUG_PORT
    digitalHi(supervisorData.debugLed);
#endif
}

void supervisorInit(void) {
    memset((void *)&supervisorData, 0, sizeof(supervisorData));

    supervisorData.readyLed = digitalInit(SUPERVISOR_READY_PORT, SUPERVISOR_READY_PIN, 0);
#ifdef SUPERVISOR_DEBUG_PORT
    supervisorData.debugLed = digitalInit(SUPERVISOR_DEBUG_PORT, SUPERVISOR_DEBUG_PIN, 0);
#endif
    supervisorData.gpsLed = digitalInit(GPS_LED_PORT, GPS_LED_PIN, 0);

    supervisorData.state = STATE_INITIALIZING;
    supervisorData.systemStatus = SPVR_AQ_STATUS_INIT;
    supervisorTaskStack = aqStackInit(SUPERVISOR_STACK_SIZE, "SUPERVISOR");

    supervisorData.supervisorTask = CoCreateTask(supervisorTaskCode, (void *)0, SUPERVISOR_PRIORITY, &supervisorTaskStack[SUPERVISOR_STACK_SIZE-1], SUPERVISOR_STACK_SIZE);
}
