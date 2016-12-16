/************************************************************************
*
Legrand MyHome / OpenWebNet Interface board driver for Domoticz
Date: 24-01-2016
Written by: Stéphane Lebrasseur

Date: 04-11-2016
Update by: Matteo Facchetti

License: Public domain


 ************************************************************************/
#include "stdafx.h"
#include "OpenWebNet.h"
#include "openwebnet/bt_openwebnet.h"

#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/SQLHelper.h"
#include "../main/localtime_r.h"
#include "csocket.h"

#include <string.h>
#include "hardwaretypes.h"
#include "../main/RFXNames.h"
#include "../main/RFXtrx.h"

#define OPENWEBNET_HEARTBEAT_DELAY 1
#define OPENWEBNET_STATUS_NB_HEARTBEAT 600
#define OPENWEBNET_RETRY_DELAY 30
#define OPENWEBNET_POLL_INTERVAL 1000
#define OPENWEBNET_BUFFER_SIZE 1024
#define OPENWEBNET_SOCKET_SUCCESS 0
#define OPENWEBNET_AUTOMATION "AUTOMATION"
#define OPENWEBNET_LIGHT "LIGHT"
#define OPENWEBNET_TEMPERATURE "TEMPERATURE"

/**
    Create new hardware OpenWebNet instance
**/
COpenWebNet::COpenWebNet(const int ID, const std::string &IPAddress, const unsigned short usIPPort) : m_szIPAddress(IPAddress)
{
	m_HwdID = ID;
	m_stoprequested = false;
	m_usIPPort = usIPPort;
	m_heartbeatcntr = OPENWEBNET_HEARTBEAT_DELAY;
	m_pStatusSocket = NULL;
}

/**
    destroys hardware OpenWebNet instance
**/
COpenWebNet::~COpenWebNet(void)
{
}

/**
    Start Hardware OpneWebNet Monitor/Worker Service
**/
bool COpenWebNet::StartHardware()
{
	m_stoprequested = false;
	m_bIsStarted = true;
    firstscan = false;

	//Start monitor thread
	m_monitorThread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&COpenWebNet::MonitorFrames, this)));

	//Start worker thread
	if (m_monitorThread != NULL) {
		m_heartbeatThread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&COpenWebNet::Do_Work, this)));
	}

	return (m_monitorThread!=NULL && m_heartbeatThread!=NULL);
}

/**
    Stop Hardware OpneWebNet Monitor/Worker Service
**/
bool COpenWebNet::StopHardware()
{
	m_stoprequested = true;

    _log.Log(LOG_STATUS, "COpenWebNet: StopHardware");
    if (isStatusSocketConnected())
	{
		try {
			disconnect();  // disconnet socket if present
		}
		catch (...)
		{
			//Don't throw from a Stop command
		}
	}

	try {
		if (m_monitorThread)
		{
		}
		if (m_heartbeatThread)
		{
		}
	}
	catch (...)
	{
		//Don't throw from a Stop command
	}
	m_bIsStarted = false;
	firstscan = false;
	return true;
}

/**
    Close and delete the socket
**/
void COpenWebNet::disconnect()
{
	if (m_pStatusSocket != NULL)
    {
        _log.Log(LOG_STATUS, "COpenWebNet: disconnect");
        if (m_pStatusSocket->getState() != csocket::CLOSED)
            m_pStatusSocket->close();
		delete m_pStatusSocket;
		m_pStatusSocket = NULL;
	}
}


/**
   Check socket connection
**/
bool COpenWebNet::isStatusSocketConnected()
{
	return m_pStatusSocket!=NULL && m_pStatusSocket->getState() == csocket::CONNECTED;
};

/**
   Connection to the gateway OpenWebNet
**/
bool COpenWebNet::connectGwOwn()
{
	disconnect();  // disconnet socket if present

	if (m_szIPAddress.size() == 0 || m_usIPPort == 0 || m_usIPPort > 65535)
	{
		_log.Log(LOG_ERROR, "COpenWebNet: Cannot connect to gateway, empty  IP Address or Port");
		return false;
	}

	m_pStatusSocket = new csocket();
	m_pStatusSocket->connect(m_szIPAddress.c_str(), m_usIPPort);

	if (m_pStatusSocket->getState() != csocket::CONNECTED)
	{
		_log.Log(LOG_ERROR, "COpenWebNet: Cannot connect to gateway, Unable to connect to specified IP Address on specified Port");
		disconnect();  // disconnet socket if present
		return false;
	}

	_log.Log(LOG_STATUS, "COpenWebNet: connected to: %s:%ld", m_szIPAddress.c_str(), m_usIPPort);

    int bytesWritten = m_pStatusSocket->write(OPENWEBNET_EVENT_SESSION, strlen(OPENWEBNET_EVENT_SESSION));
	if (bytesWritten != strlen(OPENWEBNET_EVENT_SESSION)) {
		_log.Log(LOG_ERROR, "COpenWebNet: partial write");
	}


	char databuffer[OPENWEBNET_BUFFER_SIZE];
	memset(databuffer, 0, OPENWEBNET_BUFFER_SIZE);
	int read = m_pStatusSocket->read(databuffer, OPENWEBNET_BUFFER_SIZE, false);
	bt_openwebnet responseSession(string(databuffer, read));
	if (!responseSession.IsOKFrame())
    {
		_log.Log(LOG_STATUS, "COpenWebNet: failed to begin session, NACK received (%s)", m_szIPAddress.c_str(), m_usIPPort, databuffer);
        disconnect();  // disconnet socket if present
		return false;
	}

	sOnConnected(this);
	return true;
}

/**
    Thread Monitor: get update from the OpenWebNet gateway and add new devices if necessary
**/
void COpenWebNet::MonitorFrames()
{
	//TODO : monitor socket is closed every 1 hour : replace socket before it closes
	while (!m_stoprequested)
	{
	    if (!isStatusSocketConnected())
        {
            if (m_stoprequested) break;
            time_t atime=time(NULL);
			if ((atime%OPENWEBNET_RETRY_DELAY)==0)
			{
			    if(!connectGwOwn())
                    _log.Log(LOG_STATUS, "COpenWebNet: TCP/IP monitor not connected, retrying in %d seconds...", OPENWEBNET_RETRY_DELAY);
			}
        }
        else
		{
			char data[OPENWEBNET_BUFFER_SIZE];
			memset(data, 0, OPENWEBNET_BUFFER_SIZE);
			int bread = m_pStatusSocket->read(data, OPENWEBNET_BUFFER_SIZE, false);

			if (m_stoprequested) break;
			m_LastHeartbeat = mytime(NULL);

			if ((bread == 0) || (bread<0)) {
				_log.Log(LOG_ERROR, "COpenWebNet: TCP/IP monitor connection closed!");
				disconnect();  // disconnet socket if present
			}
			else
			{
				boost::lock_guard<boost::mutex> l(readQueueMutex);
				vector<bt_openwebnet> responses;
				ParseData(data, bread, responses);

				for (vector<bt_openwebnet>::iterator iter = responses.begin(); iter != responses.end(); iter++) {
					if (iter->IsNormalFrame() || iter->IsMeasureFrame())
                    {
                        _log.Log(LOG_STATUS, "COpenWebNet: received=%s", frameToString(*iter).c_str());
                        UpdateDeviceValue(iter);

					}
					//else
                    //    _log.Log(LOG_ERROR, "COpenWebNet: SKIPPED FRAME=%s", frameToString(*iter).c_str());
				}
			}
		}
	}
	_log.Log(LOG_STATUS, "COpenWebNet: TCP/IP monitor worker stopped...");
}

/**
    Insert/Update temperature device
**/
void COpenWebNet::UpdateTemp(const int who, const int where, float fval, const int BatteryLevel, const char *devname)
{
    int cnode =  ((who << 12) & 0xF000) | (where & 0xFFF);
    SendTempSensor(cnode, BatteryLevel, fval, devname);
}



/**
    Insert/Update blinds device
**/
void COpenWebNet::UpdateBlinds(const int who, const int where, const int Command, const int BatteryLevel, const char *devname)
{
    //make device ID
    unsigned char ID1 = (unsigned char)((who & 0xFF00) >> 8);
	unsigned char ID2 = (unsigned char)(who & 0xFF);
	unsigned char ID3 = (unsigned char)((where & 0xFF00) >> 8);
	unsigned char ID4 = (unsigned char)where & 0xFF;

    char szIdx[10];
	sprintf(szIdx, "%02X%02X%02X%02X", ID1, ID2, ID3, ID4);

	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT nValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit==%d)",
                           m_HwdID, szIdx, 0);
	if (!result.empty())
	{
        //check if we have a change, if not do not update it
        int nvalue = atoi(result[0][0].c_str());
        if (Command == nvalue) return;
	}
	else
    {
        // Special insert to set SwitchType = STYPE_VenetianBlindsEU
        // so we have stop button!
        m_sql.safe_query("INSERT INTO DeviceStatus (HardwareID, DeviceID, Unit, Type, SubType, SwitchType, Name, Used) "
                         "VALUES (%d,'%s',0,%d,%d,%d,'%q',0)",
                         m_HwdID, szIdx, pTypeGeneralSwitch, sSwitchBlindsT1, STYPE_VenetianBlindsEU, devname);
    }

    _tGeneralSwitch gswitch;
    gswitch.subtype = sSwitchBlindsT1;
    gswitch.id = (((int32_t)who << 16) & 0xFF0000) | (where & 0xFFFF);
    gswitch.unitcode = 0;
    gswitch.cmnd = Command;
    gswitch.level = 100;
    gswitch.battery_level = BatteryLevel;
    gswitch.rssi = 12;
    gswitch.seqnbr = 0;
    sDecodeRXMessage(this, (const unsigned char *)&gswitch, devname, BatteryLevel);
}

/**
    Insert/Update  switch device
**/
void COpenWebNet::UpdateSwitch(const int who, const int where, const int Command, const double Level, const int BatteryLevel, const char *devname)
{

    //make device ID
	unsigned char ID1 = (unsigned char)((who & 0xFF00) >> 8);
	unsigned char ID2 = (unsigned char)(who & 0xFF);
	unsigned char ID3 = (unsigned char)((where & 0xFF00) >> 8);
	unsigned char ID4 = (unsigned char)(where & 0xFF);

	char szIdx[10];
	sprintf(szIdx, "%02X%02X%02X%02X", ID1, ID2, ID3, ID4);


    double rlevel = (15.0 / 100)*Level;
	int level = int(rlevel);

	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT nValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit==%d)",
                            m_HwdID, szIdx, 0);
	if (!result.empty())
	{
        //check if we have a change, if not do not update it
        int nvalue = atoi(result[0][0].c_str());
        if (Command == nvalue) return;
	}

    _tGeneralSwitch gswitch;
    gswitch.subtype = sSwitchLightT1;
    gswitch.id = (int32_t) (((int32_t)ID1 << 24) & 0xFF000000) | (((int32_t)ID2 << 16) & 0xFF0000) | (((int32_t)ID3 << 8) & 0xFF00) | ((int32_t)ID4 & 0xFF);
    gswitch.unitcode = 0;
    gswitch.cmnd = Command;
    gswitch.level = 100;
    gswitch.battery_level = BatteryLevel;
    gswitch.rssi = 12;
    gswitch.seqnbr = 0;
    sDecodeRXMessage(this, (const unsigned char *)&gswitch, devname, BatteryLevel);
}

void COpenWebNet::UpdateDeviceValue(vector<bt_openwebnet>::iterator iter)
{
    string who = iter->Extract_who();
    string where = iter->Extract_where();
    string what = iter->Extract_what();
    string dimension = iter->Extract_dimension();
    string value = iter->Extract_value(0);
    string devname;

    switch (atoi(who.c_str())) {
        case WHO_LIGHTING:
            if(!iter->IsNormalFrame())
            {
                _log.Log(LOG_ERROR, "COpenWebNet: Who=%s frame error!", who.c_str());
                return;
            }
            devname = OPENWEBNET_LIGHT;
            devname += " " + where;                            // 1
			//pTypeGeneralSwitch, sSwitchLightT1
            UpdateSwitch(WHO_LIGHTING, atoi(where.c_str()), atoi(what.c_str()) ? gswitch_sOn : gswitch_sOff, 100., 100, devname.c_str());
            break;
        case WHO_AUTOMATION:
            if(!iter->IsNormalFrame())
            {
                _log.Log(LOG_ERROR, "COpenWebNet: Who=%s frame error!", who.c_str());
                return;
            }
            int app_value;
            switch(atoi(what.c_str()))
            {
            case AUTOMATION_WHAT_STOP:  // 0
                app_value = gswitch_sStop;
                break;
            case AUTOMATION_WHAT_UP:    // 1
                app_value = gswitch_sOff;
                break;
            case AUTOMATION_WHAT_DOWN:  // 2
                app_value = gswitch_sOn;
                break;
            default:
                return;
            }
            devname = OPENWEBNET_AUTOMATION;
            devname += " " + where;
			//pTypeGeneralSwitch, sSwitchBlindsT1
            UpdateBlinds(WHO_AUTOMATION, atoi(where.c_str()), app_value, 100, devname.c_str());                       // 2
            break;
        case WHO_TEMPERATURE_CONTROL:
            if(!iter->IsMeasureFrame())
            {
                _log.Log(LOG_ERROR, "COpenWebNet: Who=%s frame error!", who.c_str());
                return;
            }             // 4
            if (atoi(dimension.c_str()) == 0)
            {
                devname = OPENWEBNET_TEMPERATURE;
                devname += " " + where;
                UpdateTemp(WHO_TEMPERATURE_CONTROL, atoi(where.c_str()), atof(value.c_str()) / 10., 100, devname.c_str());
            }

            else
                _log.Log(LOG_STATUS, "COpenWebNet: who=%s, where=%s, dimension=%s not yet supported", who.c_str(), where.c_str(), dimension.c_str());
            break;
        case WHO_SCENARIO:                              // 0
        case WHO_LOAD_CONTROL:                          // 3
        case WHO_BURGLAR_ALARM:                         // 5
        case WHO_DOOR_ENTRY_SYSTEM:                     // 6
        case WHO_MULTIMEDIA:                            // 7
        case WHO_AUXILIARY:                             // 9
        case WHO_GATEWAY_INTERFACES_MANAGEMENT:         // 13
        case WHO_LIGHT_SHUTTER_ACTUATOR_LOCK:           // 14
        case WHO_SCENARIO_SCHEDULER_SWITCH:             // 15
        case WHO_AUDIO:                                 // 16
        case WHO_SCENARIO_PROGRAMMING:                  // 17
        case WHO_ENERGY_MANAGEMENT:                     // 18
        case WHO_LIHGTING_MANAGEMENT:                   // 24
        case WHO_SCENARIO_SCHEDULER_BUTTONS:            // 25
        case WHO_DIAGNOSTIC:                            // 1000
        case WHO_AUTOMATIC_DIAGNOSTIC:                  // 1001
        case WHO_THERMOREGULATION_DIAGNOSTIC_FAILURES:  // 1004
        case WHO_DEVICE_DIAGNOSTIC:                     // 1013
            _log.Log(LOG_ERROR, "COpenWebNet: Who=%s not yet supported!", who.c_str());
            return;
    default:
            _log.Log(LOG_ERROR, "COpenWebNet: ERROR Who=%s not exist!", who.c_str());
        return;
    }

}


/**
   Convert domoticz command in a OpenWebNet command, then send it to device
**/
bool COpenWebNet:: WriteToHardware(const char *pdata, const unsigned char length)
{
	_tGeneralSwitch *pCmd = (_tGeneralSwitch*)pdata;

	unsigned char packetlength = pCmd->len;
	unsigned char packettype = pCmd->type;
	unsigned char subtype = pCmd->subtype;

    int who = 0;
	int what = 0;
	int where = 0;

	// Test packet type
	switch(packettype){
        case pTypeGeneralSwitch:
            // Test general switch subtype
            switch(subtype){
                case sSwitchBlindsT1:
                    //Blinds/Window command
                    who = WHO_AUTOMATION;
                	//where = ((int)(pCmd->BLINDS1.id2 << 8) & 0xFF00) +  ((int)pCmd->BLINDS1.id3 & 0xFF);
                	where = (int)(pCmd->id & 0xFFFF);


                    if (pCmd->cmnd == gswitch_sOff)
                    {
                        what = AUTOMATION_WHAT_UP;
                    }
                    else if (pCmd->cmnd == gswitch_sOn)
                    {
                        what = AUTOMATION_WHAT_DOWN;
                    }
                    else if (pCmd->cmnd == gswitch_sStop)
                    {
                        what = AUTOMATION_WHAT_STOP;
                    }
                    break;
                case sSwitchLightT1:
                    //Light/Switch command
                    who = WHO_LIGHTING;
                	//where = ((int)(pCmd->LIGHTING2.id3 << 8) & 0xFF00) +  ((int)pCmd->LIGHTING2.id4 & 0xFF);
                	where = (int)(pCmd->id & 0xFFFF);

                    if (pCmd->cmnd == gswitch_sOff)
                    {
                        what = LIGHT_WHAT_OFF;
                    }
                    else if (pCmd->cmnd == gswitch_sOn)
                    {
                        what = LIGHT_WHAT_ON;
                    }
                default:
                    break;
            }
            break;
        case pTypeThermostat:
            // Test Thermostat subtype
            switch(subtype){
                case sTypeThermSetpoint:
                case sTypeThermTemperature:
                    break;
                default:
                    break;
            }
            break;

	default:
		_log.Log(LOG_STATUS, "COpenWebNet unknown command: packettype=%d subtype=%d", packettype, subtype);
		return false;
	}

	int used = 1;
	if (!FindDevice(who, where, &used)) {
		_log.Log(LOG_ERROR, "COpenWebNet: command received for unknown device : %d/%d", who, where);
		return false;
	}

	vector<bt_openwebnet> responses;
	bt_openwebnet request(who, where, what);
	if (sendCommand(request, responses))
	{
		if (responses.size() > 0)
		{
			return responses.at(0).IsOKFrame();
		}
	}

	return true;
}

/**
   Send OpenWebNet command to device
**/
bool COpenWebNet::sendCommand(bt_openwebnet& command, vector<bt_openwebnet>& response, int waitForResponse, bool silent)
{
	csocket commandSocket;

	if (m_szIPAddress.size() == 0 || m_usIPPort == 0 || m_usIPPort > 65535)
	{
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNet: Cannot connect to gateway, empty IP Address or Port");
		}
		return false;
	}

	int connectResult = commandSocket.connect(m_szIPAddress.c_str(), m_usIPPort);
	if (connectResult != OPENWEBNET_SOCKET_SUCCESS) {
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNet: Cannot connect to gateway : %d", connectResult);
		}
		return false;
	}

	if (commandSocket.getState() != csocket::CONNECTED)
	{
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNet: Cannot connect to gateway, Unable to connect to specified IP Address on specified Port");
		}
		return false;
	}

	int bytesWritten = commandSocket.write(OPENWEBNET_COMMAND_SESSION, strlen(OPENWEBNET_COMMAND_SESSION));
	if (bytesWritten != strlen(OPENWEBNET_COMMAND_SESSION)) {
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNet sendCommand: partial write");
		}
	}

	char databuffer[OPENWEBNET_BUFFER_SIZE];
	memset(databuffer, 0, OPENWEBNET_BUFFER_SIZE);
	int read = commandSocket.read(databuffer, OPENWEBNET_BUFFER_SIZE, false);
	bt_openwebnet responseSession(string(databuffer, read));

	if (!responseSession.IsOKFrame()) {
		if (!silent) {
			_log.Log(LOG_STATUS, "COpenWebNet: failed to begin session, NACK received (%s)", m_szIPAddress.c_str(), m_usIPPort, databuffer);
		}
		return false;
	}

	bytesWritten = commandSocket.write(command.frame_open.c_str(), command.frame_open.length());
	if (bytesWritten != command.frame_open.length()) {
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNet sendCommand: partial write");
		}
	}

	if (waitForResponse > 0) {
		sleep_seconds(waitForResponse);
	}

	char responseBuffer[OPENWEBNET_BUFFER_SIZE];
	memset(responseBuffer, 0, OPENWEBNET_BUFFER_SIZE);
	read = commandSocket.read(responseBuffer, OPENWEBNET_BUFFER_SIZE, false);

	if (!silent) {
		_log.Log(LOG_STATUS, "COpenWebNet: sent=%s received=%s", command.frame_open.c_str(), responseBuffer);
	}

    if (commandSocket.getState() != csocket::CLOSED)
        commandSocket.close();

    boost::lock_guard<boost::mutex> l(readQueueMutex);
	return ParseData(responseBuffer, read, response);
}

/**
    automatic scan of automation/lighting device
**/
void COpenWebNet::scan_automation_lighting()
{
    bt_openwebnet request;
    vector<bt_openwebnet> responses;
    stringstream whoStr;
    stringstream whereStr;
	whoStr << WHO_LIGHTING;
    whereStr << 0;
    request.CreateStateMsgOpen(whoStr.str(), whereStr.str());
    sendCommand(request, responses, 0, true);

}

/**
    automatic scan of temperature control device
**/
void COpenWebNet::scan_temperature_control()
{
    bt_openwebnet request;
    vector<bt_openwebnet> responses;
	stringstream whoStr;
	stringstream dimensionStr;
	whoStr << WHO_TEMPERATURE_CONTROL;
    dimensionStr << 0;

    for (int where = 1; where < 100; where++)
    {
        stringstream whereStr;
        whereStr << where;
        request.CreateDimensionMsgOpen(whoStr.str(), whereStr.str(), dimensionStr.str());
        sendCommand(request, responses, 0, true);
    }
}
/**
    Request time to gateway
**/
void COpenWebNet::requestTime()
{
    _log.Log(LOG_STATUS, "COpenWebNet: request time...");
    bt_openwebnet request;
    vector<bt_openwebnet> responses;
    request.CreateTimeReqMsgOpen();
    sendCommand(request, responses, 0, true);
}

void COpenWebNet::scan_device()
{
    /* uncomment the line below to enable the time request to the gateway.
    Note that this is only for debugging, the answer to who = 13 is not yet supported */
    //requestTime();
    _log.Log(LOG_STATUS, "COpenWebNet: scanning automation/lighting...");
    scan_automation_lighting();
    _log.Log(LOG_STATUS, "COpenWebNet: scanning temperature control...");
    scan_temperature_control();
    _log.Log(LOG_STATUS, "COpenWebNet: scan device complete, wait all the update data..");
}

bool COpenWebNet::ParseData(char* data, int length, vector<bt_openwebnet>& messages)
{
	string buffer = string(data, length);
	size_t begin = 0;
	size_t end = string::npos;
	do {
		end = buffer.find(OPENWEBNET_END_FRAME, begin);
		if (end != string::npos) {
			bt_openwebnet message(buffer.substr(begin, end - begin + 2));
			messages.push_back(message);
			begin = end + 2;
		}
	} while (end != string::npos);

	return true;
}

void COpenWebNet::Do_Work()
{
	while (!m_stoprequested)
	{
	    if (isStatusSocketConnected() && !firstscan)
        {
            firstscan = true;
            _log.Log(LOG_STATUS, "COpenWebNet: start scan devices...");
            scan_device();
            _log.Log(LOG_STATUS, "COpenWebNet: scan devices complete.");
        }
		sleep_seconds(OPENWEBNET_HEARTBEAT_DELAY);
		m_LastHeartbeat = mytime(NULL);
	}
	_log.Log(LOG_STATUS, "COpenWebNet: Heartbeat worker stopped...");
}

/**
   Find OpenWebNetDevice in DB
**/
bool COpenWebNet::FindDevice(int who, int where, int* used)
{
	vector<vector<string> > result;
	int devType = -1;
	int subType = -1;
	int subUnit = 0;

    		//make device ID
    unsigned char ID1 = (unsigned char)((who & 0xFF00) >> 8);
	unsigned char ID2 = (unsigned char)(who & 0xFF);
	unsigned char ID3 = (unsigned char)((where & 0xFF00) >> 8);
	unsigned char ID4 = (unsigned char)(where & 0xFF);

	char szIdx[10];
	switch (who) {
        case WHO_LIGHTING:                              // 1
		    //devType = pTypeLighting2;
			//subType = sTypeLightMyHome;
			devType = pTypeGeneralSwitch;
			subType = sSwitchLightT1;
			sprintf(szIdx, "%02X%02X%02X%02X", ID1, ID2, ID3, ID4);
            break;
		case WHO_AUTOMATION:                            // 2
            //devType = pTypeBlinds;
            //subType = sTypeBlindsT13;
			devType = pTypeGeneralSwitch;
            subType = sSwitchBlindsT1;
            sprintf(szIdx, "%02X%02X%02X%02X", ID1, ID2, ID3, ID4);
            break;
        case WHO_TEMPERATURE_CONTROL:                   // 4
            //devType = pTypeGeneral;
            //subType = sTypeTemperature;
            //subUnit = where;
            //printf(szIdx, "%02X%02X", who, where);
            //break;
			return true; // device always present
        case WHO_SCENARIO:                              // 0
		case WHO_LOAD_CONTROL:                          // 3
		case WHO_BURGLAR_ALARM:                         // 5
		case WHO_DOOR_ENTRY_SYSTEM:                     // 6
		case WHO_MULTIMEDIA:                            // 7
		case WHO_AUXILIARY:                             // 9
		case WHO_GATEWAY_INTERFACES_MANAGEMENT:         // 13
		case WHO_LIGHT_SHUTTER_ACTUATOR_LOCK:           // 14
		case WHO_SCENARIO_SCHEDULER_SWITCH:             // 15
		case WHO_AUDIO:                                 // 16
		case WHO_SCENARIO_PROGRAMMING:                  // 17
		case WHO_ENERGY_MANAGEMENT:                     // 18
		case WHO_LIHGTING_MANAGEMENT:                   // 24
		case WHO_SCENARIO_SCHEDULER_BUTTONS:            // 25
		case WHO_DIAGNOSTIC:                            // 1000
		case WHO_AUTOMATIC_DIAGNOSTIC:                  // 1001
		case WHO_THERMOREGULATION_DIAGNOSTIC_FAILURES:  // 1004
		case WHO_DEVICE_DIAGNOSTIC:                     // 1013
	default:
			return false;
	}

    if ((who == WHO_LIGHTING) || (who == WHO_AUTOMATION) || (who == WHO_TEMPERATURE_CONTROL))
    {
        if (used != NULL)
        {
            result = m_sql.safe_query("SELECT ID FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit == %d) AND (Type==%d) AND (Subtype==%d) and Used == %d",
                    m_HwdID, szIdx, subUnit, devType, subType, *used);
        }
        else
        {
            result = m_sql.safe_query("SELECT ID FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit == %d) AND (Type==%d) AND (Subtype==%d)",
                m_HwdID, szIdx, subUnit, devType, subType);
        }
    }
    else
        return false;


	if (result.size() > 0)
	{
		return true;
	}

	return false;
}

/**
   Convert the frame in string a string
**/
string COpenWebNet::frameToString(bt_openwebnet& frame)
{
	stringstream frameStr;

	frameStr << frame.frame_open;
	frameStr << " : ";

	if (frame.IsErrorFrame())
	{
		frameStr << "ERROR FRAME";
	}
	else if(frame.IsNullFrame())
	{
		frameStr << "NULL FRAME";
	}
	else if (frame.IsMeasureFrame())
	{
		frameStr << "MEASURE FRAME";
	}
	else if (frame.IsStateFrame())
	{
		frameStr << "STATE FRAME";
	}
	else if (frame.IsWriteFrame())
	{
		frameStr << "WRITE FRAME";
	}
	else if (frame.IsPwdFrame())
	{
		frameStr << "PASSWORD FRAME";
	}
	else if (frame.IsOKFrame())
	{
		frameStr << "ACK FRAME";
	}
	else if (frame.IsKOFrame())
	{
		frameStr << "NACK FRAME";
	}
	else if (frame.IsNormalFrame())
	{
		frameStr << "NORMAL FRAME";

		if (frame.extended) {
			frameStr << " - EXTENDED";
		}

		frameStr << " - who=" << getWhoDescription(frame.Extract_who());
		frameStr << " - what=" << getWhatDescription(frame.Extract_who(), frame.Extract_what());
		frameStr << " - where=" << frame.Extract_where();
		if (!frame.Extract_when().empty()) {
			frameStr << " - when=" << frame.Extract_when();
		}
		if (!frame.Extract_level().empty()) {
			frameStr << " - level=" << frame.Extract_level();
		}
		if (!frame.Extract_interface().empty()) {
			frameStr << " - interface=" << frame.Extract_interface();
		}
		if (!frame.Extract_dimension().empty()) {
			frameStr << " - dimension=" << frame.Extract_dimension();
		}

		string indirizzo = frame.Extract_address(0);
		if (!indirizzo.empty()) {
			int i = 1;
			frameStr << " - address=";
			while (!indirizzo.empty()) {
				frameStr << indirizzo;
				indirizzo = frame.Extract_address(i++);
				if (!indirizzo.empty()) {
					frameStr << ", ";
				}
			}
		}

		string valori = frame.Extract_value(0);
		if (!valori.empty()) {
			int i = 1;
			frameStr << " - value=";
			while (!valori.empty()) {
				frameStr << valori;
				indirizzo = frame.Extract_value(i++);
				if (!valori.empty()) {
					frameStr << ", ";
				}
			}
		}
	}

	return frameStr.str();
}

/**
    Get a string Description of WHO
**/
string COpenWebNet::getWhoDescription(string who)
{
	if (who == "0") {
		return "Scenario";
	}
	if (who == "1") {
		return "Lighting";
	}
	if (who == "2") {
		return "Automation";
	}
	if (who == "3") {
		return "Load control";
	}
	if (who == "4") {
		return "Temperature control";
	}
	if (who == "5") {
		return "Burglar alarm";
	}
	if (who == "6") {
		return "Door entry system";
	}
	if (who == "7") {
		return "Multimedia";
	}
	if (who == "9") {
		return "Auxiliary";
	}
	if (who == "13") {
		return "Gateway interfaces management";
	}
	if (who == "14") {
		return "Light shutter actuator lock";
	}
	if (who == "15") {
		return "Scenario Scheduler Switch";
	}
	if (who == "16") {
		return "Audio";
	}
	if (who == "17") {
		return "Scenario programming";
	}
	if (who == "18") {
		return "Energy management";
	}
	if (who == "24") {
		return "Lighting management";
	}
	if (who == "25") {
		return "Scenario scheduler buttons";
	}
	if (who == "1000") {
		return "Diagnostic";
	}
	if (who == "1001") {
		return "Automation diagnostic";
	}
	if (who == "1004") {
		return "Thermoregulation diagnostic failure";
	}
	if (who == "1013") {
		return "Device diagnostic";
	}

	return who;
}

/**
    Get a string Description of WHAT
**/
string COpenWebNet::getWhatDescription(string who, string what)
{
	if (who == "0") {
		// "Scenario";
	}
	if (who == "1") {
		// "Lighting";
	}
	if (who == "2") {
		// "Automation";
		if (what == "0") {
			return "Stop";
		}
		if (what == "1") {
			return "Up";
		}
		if (what == "2") {
			return "Down";
		}
	}
	if (who == "3") {
		// "Load control";
	}
	if (who == "4") {
		// "Temperature control";
	}
	if (who == "5") {
		// "Burglar alarm";
	}
	if (who == "6") {
		// "Door entry system";
	}
	if (who == "7") {
		// "Multimedia";
	}
	if (who == "9") {
		// "Auxiliary";
	}
	if (who == "13") {
		// "Gateway interfaces management";
	}
	if (who == "14") {
		// "Light shutter actuator lock";
	}
	if (who == "15") {
		// "Scenario Scheduler Switch";
	}
	if (who == "16") {
		// "Audio";
	}
	if (who == "17") {
		// "Scenario programming";
	}
	if (who == "18") {
		// "Energy management";
	}
	if (who == "24") {
		// "Lighting management";
	}
	if (who == "25") {
		// "Scenario scheduler buttons";
	}
	if (who == "1000") {
		// "Diagnostic";
	}
	if (who == "1001") {
		// "Automation diagnostic";
	}
	if (who == "1004") {
		// "Thermoregulation diagnostic failure";
	}
	if (who == "1013") {
		// "Device diagnostic";
	}

	return what;
}

