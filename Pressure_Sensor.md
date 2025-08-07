Project: Remote Monitoring of a VERIS PX3PXX01 Pressure SensorThis project outlines the steps to power and read data from a VERIS PX3PXX01 dry pressure sensor. The sensor, along with a listening device, will be powered by a shared source. The listening device will interpret the sensor's analog output and transmit the pressure data to a third-party cloud service for remote monitoring.1. System ArchitectureThe system consists of four main components:VERIS PX3PXX01 Pressure Sensor: The primary sensing element.Listening Device: A Raspberry Pi 4 with a 4-20mA HAT responsible for reading the sensor's analog signal, converting it to a pressure value, and sending it to the cloud.Shared Power Supply: A single 24VDC power source will power both the VERIS sensor and the Raspberry Pi.Third-Party IoT Service: Ubidots will be used to receive, store, and visualize the pressure data.System Diagram:      +---------------------+      +-------------------------+
      | 24VDC Power Supply  |----->|   VERIS PX3PXX01        |
      +---------------------+      | (4-20mA Output)         |
               |                   +-------------------------+
               |                              | 4-20mA Signal
               |                              |
               v                              v
      +---------------------+      +-------------------------+
      |  Raspberry Pi 4     |<-----|   4-20mA HAT            |
      | (Listening Device)  |      +-------------------------+
      +---------------------+
               |
               | Wi-Fi
               v
      +---------------------+
      |   Ubidots Cloud     |
      +---------------------+
2. Hardware RequirementsComponentDescriptionQuantityVERIS PX3PXX01Dry Pressure Sensor1Raspberry Pi 4Microcontroller14-20mA HAT for Raspberry PiReads 4-20mA analog signals124VDC Power SupplyTo power the sensor and Pi1DC-DC Buck ConverterTo step down 24VDC to 5VDC for the Pi1Jumper Wires & ConnectorsFor wiringAs neededEnclosureTo house the components13. Software and Configuration3.1. VERIS PX3PXX01 ConfigurationThe sensor needs to be configured for a 4-20mA output. This is typically done using DIP switches on the device. Refer to the VERIS PX3PXX01 datasheet for the specific switch settings for the desired pressure range and 4-20mA output.3.2. Raspberry Pi SetupInstall Raspberry Pi OS: Flash the latest version of Raspberry Pi OS onto a microSD card.Enable I2C: The 4-20mA HAT communicates with the Raspberry Pi via the I2C interface. This needs to be enabled using the raspi-config tool.Install HAT Software: Install the Python libraries provided by the manufacturer of the 4-20mA HAT.Install Ubidots Library:pip install ubidots
3.3. Python ApplicationA Python script will be developed to perform the following tasks:Initialize the 4-20mA HAT.Read the 4-20mA signal.Convert the current to a pressure value.Send the data to Ubidots.Example Python Snippet (Conceptual):import time
from NCD_4_20mA_HAT import NCD_4_20mA
from ubidots import ApiClient

# --- Ubidots Configuration ---
UBIDOTS_TOKEN = "YOUR_UBIDOTS_TOKEN"
DEVICE_LABEL = "pressure-monitor"
VARIABLE_LABEL = "pressure"

# --- Initialize HAT and Ubidots API ---
hat = NCD_4_20mA()
api = ApiClient(token=UBIDOTS_TOKEN)
pressure_variable = api.get_variable(VARIABLE_LABEL)

def read_pressure():
    # Read current from channel 1
    current_mA = hat.read_channel_current(1)
    
    # Convert current to pressure (assuming 0-1" WC range)
    # This formula needs to be adjusted based on the sensor's range
    pressure_wc = (current_mA - 4.0) * (1.0 / 16.0) 
    
    return pressure_wc

while True:
    try:
        pressure = read_pressure()
        print(f"Pressure: {pressure:.2f} \"WC")
        
        # Send data to Ubidots
        pressure_variable.save_value({'value': pressure})
        
    except Exception as e:
        print(f"An error occurred: {e}")

    time.sleep(60) # Send data every minute
4. Power DistributionA single 24VDC power supply will be used.VERIS PX3PXX01: The sensor will be directly powered by the 24VDC supply.Raspberry Pi 4: A DC-DC buck converter will be used to step down the 24VDC to a stable 5VDC to power the Raspberry Pi via its USB-C port or GPIO pins.Wiring Considerations:Ensure correct polarity when connecting the power supply to the sensor and the buck converter.The 4-20mA loop from the sensor will be connected to the input terminals of the 4-20mA HAT.5. Project TimelinePhaseTaskEstimated Time1. ProcurementOrder all hardware components.1-2 Weeks2. Setup- Configure VERIS sensor. <br>- Setup Raspberry Pi OS and software. <br>- Assemble hardware.1 Day3. Development- Develop and test Python script. <br>- Integrate Ubidots API.2-3 Days4. Testing- Test the entire system end-to-end. <br>- Calibrate readings.1 Day5. DeploymentInstall the system in its final location.1 Day6. Risks and MitigationRisk: Inaccurate pressure readings.Mitigation: Double-check the sensor's range settings and the conversion formula in the Python script. Perform a simple calibration test if possible.Risk: Power instability affecting the Raspberry Pi.Mitigation: Use a high-quality DC-DC buck converter and ensure all wiring is secure.Risk: Network connectivity issues preventing data upload to Ubidots.Mitigation: Ensure the Raspberry Pi has a stable Wi-Fi connection. Implement error handling in the Python script to manage temporary network outages.