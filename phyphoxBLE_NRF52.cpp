#include "phyphoxBLE_NRF52.h"

const UUID PhyphoxBLE::phyphoxExperimentServiceUUID = UUID(phyphoxBleExperimentServiceUUID);
const UUID PhyphoxBLE::experimentCharacteristicUUID = UUID(phyphoxBleExperimentCharacteristicUUID);

const UUID PhyphoxBLE::phyphoxDataServiceUUID = UUID(phyphoxBleDataServiceUUID);
const UUID PhyphoxBLE::dataCharacteristicUUID = UUID(phyphoxBleDataCharacteristicUUID);
const UUID PhyphoxBLE::data2CharacteristicUUID = UUID(phyphoxBleData2CharacteristicUUID);

const UUID PhyphoxBLE::configCharacteristicUUID = UUID(phyphoxBleConfigCharacteristicUUID);

char PhyphoxBLE::name[50] = "";

int PhyphoxBLE::activeConnections = 0;

Thread PhyphoxBLE::bleEventThread;
Thread PhyphoxBLE::transferExpThread;

uint8_t PhyphoxBLE::macAddress[6] = {0};
uint8_t PhyphoxBLE::data_package[20] = {0};
uint8_t PhyphoxBLE::config_package[CONFIGSIZE] = {0};

/*BLE stuff*/
BLE& bleInstance = BLE::Instance(BLE::DEFAULT_INSTANCE);
BLE& PhyphoxBLE::ble = bleInstance;

uint8_t PhyphoxBLE::readValue[DATASIZE] = {0};

ReadWriteArrayGattCharacteristic<uint8_t, sizeof(PhyphoxBLE::data_package)> PhyphoxBLE::dataCharacteristic{PhyphoxBLE::dataCharacteristicUUID, PhyphoxBLE::data_package, GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_NOTIFY}; //Note: Use { } instead of () google most vexing parse
ReadWriteArrayGattCharacteristic<uint8_t, sizeof(PhyphoxBLE::data_package)> PhyphoxBLE::data2Characteristic{PhyphoxBLE::data2CharacteristicUUID, PhyphoxBLE::data_package, GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_NOTIFY}; //Note: Use { } instead of () google most vexing parse
ReadWriteArrayGattCharacteristic<uint8_t, sizeof(PhyphoxBLE::config_package)> PhyphoxBLE::configCharacteristic{PhyphoxBLE::configCharacteristicUUID, PhyphoxBLE::config_package, GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_NOTIFY};

//Experiment Transfer
ReadOnlyArrayGattCharacteristic<uint8_t, sizeof(PhyphoxBLE::readValue)> PhyphoxBLE::experimentCharacteristic{PhyphoxBLE::experimentCharacteristicUUID, PhyphoxBLE::readValue, GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_NOTIFY};

EventQueue PhyphoxBLE::queue{64 * EVENTS_EVENT_SIZE};
/*end BLE stuff*/
EventQueue PhyphoxBLE::transferQueue{64 * EVENTS_EVENT_SIZE};

PhyphoxBleEventHandler PhyphoxBLE::eventHandler(bleInstance);

GattCharacteristic* PhyphoxBLE::phyphoxCharacteristics[1] = {&PhyphoxBLE::experimentCharacteristic};
GattService PhyphoxBLE::phyphoxService{PhyphoxBLE::phyphoxExperimentServiceUUID, PhyphoxBLE::phyphoxCharacteristics, sizeof(PhyphoxBLE::phyphoxCharacteristics) / sizeof(GattCharacteristic *)};

GattCharacteristic* PhyphoxBLE::phyphoxDataCharacteristics[3] = {&PhyphoxBLE::dataCharacteristic,&PhyphoxBLE::data2Characteristic, &PhyphoxBLE::configCharacteristic};
//GattCharacteristic* PhyphoxBLE::phyphoxDataCharacteristics[2] = {&PhyphoxBLE::dataCharacteristic, &PhyphoxBLE::configCharacteristic};

GattService PhyphoxBLE::phyphoxDataService{PhyphoxBLE::phyphoxDataServiceUUID, PhyphoxBLE::phyphoxDataCharacteristics, sizeof(PhyphoxBLE::phyphoxDataCharacteristics) / sizeof(GattCharacteristic *)};

uint8_t* PhyphoxBLE::data = nullptr; //this pointer points to the data the user wants to write in the characteristic
uint8_t* PhyphoxBLE::config =nullptr;
uint8_t* PhyphoxBLE::p_exp = nullptr; //this pointer will point to the byte array which holds an experiment

uint8_t PhyphoxBLE::EXPARRAY[2000] = {0};// block some storage
size_t PhyphoxBLE::expLen = 0; //try o avoid this maybe use std::array or std::vector

void (*PhyphoxBLE::configHandler)() = nullptr;

void PhyphoxBleEventHandler::onDisconnectionComplete(const ble::DisconnectionCompleteEvent &event)
{
	//#ifndef NDEBUG
	//if(printer)
	//	printer -> println("Disconnection");
	//#endif
    PhyphoxBLE::activeConnections-=1;
	ble.gap().startAdvertising(ble::LEGACY_ADVERTISING_HANDLE);
}

void PhyphoxBleEventHandler::onConnectionComplete(const ble::ConnectionCompleteEvent &event)
{

    PhyphoxBLE::activeConnections+=1;
	//#ifndef NDEBUG
	//if(printer)
	//	printer -> println("Connection with device");
	//#endif	
    
}

#ifndef NDEBUG
void PhyphoxBLE::begin(HardwareSerial* hwPrint)
{
	 printer = hwPrint;
      if(printer)
		printer->begin(9600);       
}

void PhyphoxBLE::output(const char* s)
{
	if(printer)
		printer->println(s);
}

void PhyphoxBLE::output(const uint32_t num)
{
	if(printer)
		printer -> println(num);
}

#endif



void PhyphoxBLE::when_subscription_received(GattAttribute::Handle_t handle)
{
	#ifndef NDEBUG
	output("Received a subscription");
	#endif
	bool sub_dChar = false;
	ble.gattServer().areUpdatesEnabled(experimentCharacteristic, &sub_dChar);
	
	if(sub_dChar)
	{
		transferQueue.call(transferExp);
		sub_dChar = false;
	}
	//after experiment has been transfered start advertising again
	ble.gap().startAdvertising(ble::LEGACY_ADVERTISING_HANDLE);

}
void PhyphoxBLE::configReceived(const GattWriteCallbackParams *params)
{
	if(configHandler != nullptr){
		transferQueue.call( configHandler ); 		
	}
}
void PhyphoxBLE::transferExp()
{
	BLE &ble = PhyphoxBLE::ble;
	uint8_t* exp = PhyphoxBLE::p_exp;
	size_t exp_len = PhyphoxBLE::expLen;

	#ifndef NDEBUG
	PhyphoxBLE::output("In transfer exp");
	#endif

	uint8_t header[20] = {0}; //20 byte as standard package size for ble transfer
	const char phyphox[] = "phyphox";
	uint32_t table[256];
	phyphoxBleCrc32::generate_table(table);
	uint32_t checksum = phyphoxBleCrc32::update(table, 0, exp, exp_len);
	#ifndef NDEBUG
	PhyphoxBLE::output("checksum: ");
	PhyphoxBLE::output(checksum);
	#endif
	size_t arrayLength = exp_len;
	uint8_t experimentSizeArray[4] = {0};
	experimentSizeArray[0]=  (arrayLength >> 24);
	experimentSizeArray[1]=  (arrayLength >> 16);
	experimentSizeArray[2]=  (arrayLength >> 8);
	experimentSizeArray[3]=  arrayLength; 

	uint8_t checksumArray[4] = {0};
	checksumArray[0]= (checksum >> 24) & 0xFF;
	checksumArray[1]= (checksum >> 16) & 0xFF;  
	checksumArray[2]= (checksum >> 8) & 0xFF;
	checksumArray[3]= checksum & 0xFF; 

	copy(phyphox, phyphox+7, header);
	copy(experimentSizeArray, experimentSizeArray+ 4, header + 7);
	copy(checksumArray, checksumArray +  4, header +11); 
	
	ble.gattServer().write(PhyphoxBLE::experimentCharacteristic.getValueHandle(), header, sizeof(header));
	//wait_us(30000);
    ThisThread::sleep_for(60ms);
	
	for(size_t i = 0; i < exp_len/20; ++i)
	{
		copy(exp+i*20, exp+i*20+20, header);
		ble.gattServer().write(PhyphoxBLE::experimentCharacteristic.getValueHandle(), header, sizeof(header));
		//wait_us(30000);
        ThisThread::sleep_for(60ms);
	}
	
	if(exp_len%20 != 0)
	{
		const size_t rest = exp_len%20;
		uint8_t slice[rest];
		copy(exp + exp_len - rest, exp + exp_len, slice);
		ble.gattServer().write(PhyphoxBLE::experimentCharacteristic.getValueHandle(), slice, sizeof(slice));
		//wait_us(30000);
        ThisThread::sleep_for(60ms);
	}
}

void PhyphoxBLE::bleInitComplete(BLE::InitializationCompleteCallbackContext* params)
{	
	
	ble.gattServer().onUpdatesEnabled(PhyphoxBLE::when_subscription_received);
	ble.gattServer().onDataWritten(PhyphoxBLE::configReceived);
	ble.gap().setEventHandler(&PhyphoxBLE::eventHandler);

	uint8_t _adv_buffer[ble::LEGACY_ADVERTISING_MAX_SIZE];
	ble::AdvertisingDataBuilder adv_data_builder(_adv_buffer);
	ble::AdvertisingParameters adv_parameters(ble::advertising_type_t::CONNECTABLE_UNDIRECTED, ble::adv_interval_t(ble::millisecond_t(1000)));
	adv_data_builder.setFlags();
    adv_data_builder.setLocalServiceList(mbed::make_Span(&phyphoxDataServiceUUID, 1));  //changed for test "phyphoxExperimentServiceUUID"
    ble_error_t error = adv_data_builder.setName(name);


  	#ifndef NDEBUG
    if(error == BLE_ERROR_BUFFER_OVERFLOW){
		output("BLE_ERROR_BUFFER_OVERFLOW");
    }else if(error == BLE_ERROR_NONE){
		output("BLE_ERROR_NONE");
    }else if(error == BLE_ERROR_INVALID_PARAM){
		output("BLE_ERROR_INVALID_PARAM");
    }
	#endif

    ble.gap().setAdvertisingParameters(ble::LEGACY_ADVERTISING_HANDLE, adv_parameters);
    ble.gap().setAdvertisingPayload(ble::LEGACY_ADVERTISING_HANDLE,adv_data_builder.getAdvertisingData());
    ble.gattServer().addService(phyphoxService);
    ble.gattServer().addService(phyphoxDataService);
 
	ble.gap().startAdvertising(ble::LEGACY_ADVERTISING_HANDLE);
	#ifndef NDEBUG
	output("in init");
	#endif
}

void PhyphoxBLE::start(const char* DEVICE_NAME, uint8_t* exp_pointer, size_t len)
{
	/**
	 * \brief start the server and begin advertising
	 * This methods starts the server by. By specifying
	 * exp_pointer and len you can send your own phyphox
	 * experiment to your smartphone. Otherwise a default
	 * experiment will be sent. 
	 * \param exp_pointer a pointer to an experiment 
	 * \param len the len of the experiment 
	 */
	#ifndef NDEBUG
	output("In start");
	#endif

	strncpy(name, DEVICE_NAME, 49);
    name[50] = '\0';

	if(exp_pointer != nullptr){
		p_exp = exp_pointer;
		expLen = len;
	}else{
		
      PhyphoxBleExperiment defaultExperiment;

      //View
      PhyphoxBleExperiment::View firstView;

      //Graph
      PhyphoxBleExperiment::Graph firstGraph;      //Create graph which will plot random numbers over time     
      firstGraph.setChannel(0,1);    

      firstView.addElement(firstGraph);       
      defaultExperiment.addView(firstView);
      
      addExperiment(defaultExperiment);
      
	}

	bleEventThread.start(callback(&queue, &EventQueue::dispatch_forever));
  	transferExpThread.start(callback(&transferQueue, &EventQueue::dispatch_forever));
	ble.onEventsToProcess(PhyphoxBLE::schedule_ble_events);
	ble.init(PhyphoxBLE::bleInitComplete);

    ble.gap().enablePrivacy(false);


    ble::own_address_type_t addr_type;
    ble::address_t address;
    
    ble.gap().getAddress(addr_type, address);

    memcpy(&macAddress[0],address.data(),6);
}

void PhyphoxBLE::start(uint8_t* exp_pointer, size_t len) {
    start("phyphox", exp_pointer, len);
}

void PhyphoxBLE::start(const char* DEVICE_NAME) {
    start(DEVICE_NAME, nullptr, 0);
}

void PhyphoxBLE::start() {
    start("phyphox", nullptr, 0);
}

void PhyphoxBLE::poll() {
}

void PhyphoxBLE::poll(int timeout) {
}
	
void PhyphoxBLE::write(float& value)
{
	/**
	 * \brief Write a single float into characteristic
     * The float is parsed to uint8_t* 
     * because the gattServer write method 
     * expects a pointer to uint8_t
     * \param f1 represent a float most likeley sensor data
    */
	data = reinterpret_cast<uint8_t*>(&value);
	ble.gattServer().write(dataCharacteristic.getValueHandle(), data, 4);
}


///todo: public getter for gatt server 


void PhyphoxBLE::write(float& f1, float& f2)
{
	 /**
   * \brief Write 2 floats into characteristic
   * The floats are parsed to uint8_t* 
   * because the gattServer write method e
   * expects a pointer to uint8_t
   * \param f1 and f2 represent two floats most likeley sensor data
   */

	float array[2] = {f1, f2};
	data = reinterpret_cast<uint8_t*>(array);
	ble.gattServer().write(dataCharacteristic.getValueHandle(), data, 8);
}

void PhyphoxBLE::write(float& f1, float& f2, float& f3)
{
	 /**
   * \brief Write 3 floats into characteristic
   * The floats are parsed to uint8_t* 
   * because the gattServer write method 
   * expects a pointer to uint8_t
   * \param param f1 and f2 and f3 represents three floats most likeley sensor data
   */
	float array[3] = {f1, f2, f3};
	data = reinterpret_cast<uint8_t*>(array);
	ble.gattServer().write(dataCharacteristic.getValueHandle(), data, 12);
}

void PhyphoxBLE::write(float& f1, float& f2, float& f3, float& f4)
{
	 /**
   * \brief Write 4 floats into characteristic
   * The floats are parsed to uint8_t* 
   * because the gattServer write method 
   * expects a pointer to uint8_t
   * \param param f1 and f2 and f3 and f4 represents four floats most likeley sensor data
   */
	float array[4] = {f1, f2, f3, f4};
	data = reinterpret_cast<uint8_t*>(array);
	ble.gattServer().write(dataCharacteristic.getValueHandle(), data, 16);
}
void PhyphoxBLE::write(float& f1, float& f2, float& f3, float& f4, float& f5)
{
	 /**
   * \brief Write 5 floats into characteristic
   * The floats are parsed to uint8_t* 
   * because the gattServer write method 
   * expects a pointer to uint8_t
   * \param param f1 and f2 and f3 and f4 and f5 represents five floats most likeley sensor data
   */
	float array[5] = {f1, f2, f3, f4, f5};
	data = reinterpret_cast<uint8_t*>(array);
	ble.gattServer().write(dataCharacteristic.getValueHandle(), data, 20);
}
void PhyphoxBLE::write(uint8_t *arrayPointer, unsigned int arraySize)
{
	ble.gattServer().write(dataCharacteristic.getValueHandle(), arrayPointer, arraySize);
}


void PhyphoxBLE::read(float& f1)
{	
	uint16_t configSize = 4;
	uint8_t myConfig[4];
	ble.gattServer().read(configCharacteristic.getValueHandle(), myConfig, &configSize);
	memcpy(&f1,&myConfig[0], 4);
}
void PhyphoxBLE::read(uint8_t *arrayPointer, unsigned int arraySize)
{
	uint16_t myArraySize = arraySize;
	ble.gattServer().read(configCharacteristic.getValueHandle(), arrayPointer, &myArraySize);
}


void PhyphoxBLE::addExperiment(PhyphoxBleExperiment& exp)
{
	char buffer[2000] ="";
	exp.getBytes(buffer);
	memcpy(&EXPARRAY[0],&buffer[0],strlen(buffer));
	p_exp = &EXPARRAY[0];
	expLen = strlen(buffer);
}

void PhyphoxBLE::schedule_ble_events(BLE::OnEventsToProcessCallbackContext *context) {
    PhyphoxBLE::queue.call(mbed::Callback<void()>(&context->ble, &BLE::processEvents));
}

void PhyphoxBLE::writeOld(float& f1, float& f2, float& f3, float& f4)
{
	 
	float array[4] = {f1, f2, f3, f4};
	data = reinterpret_cast<uint8_t*>(array);
	ble.gattServer().write(data2Characteristic.getValueHandle(), data, 16);
}
