import clr, time
dll = r'C:\Users\gokul\AppData\Local\Programs\Python\Python312\Lib\site-packages\WinTmp\LibreHardwareMonitorLib.dll'
clr.AddReference(dll)
import LibreHardwareMonitor.Hardware as LHM
c = LHM.Computer()
c.IsCpuEnabled = True
c.Open()
with open(r'd:\seeed\sticky\reterminal_sticky_arduino\bear_pc_monitor_pc\admin_temp_result.txt', 'w') as f:
    for i in range(3):
        time.sleep(0.5)
        for h in c.Hardware:
            h.Update()
            for s in h.Sensors:
                if 'temp' in str(s.SensorType).lower():
                    f.write(f'{s.Name} = {s.Value}\n')
c.Close()
