#!/usr/bin/env python3
"""Recovery测试前置工具：查询与MP=4；固件在MP启动分支自动完成mp_start，不执行TX/efuse。
先用OTARECOVER进入Recovery，再运行本程序。结束后释放串口给WT工具。
若WT_INSERT_DUT会复位或断电，此准备结果失效，必须调整WT流程。
依赖：python -m pip install pyserial
示例：python prepare_mp.py --port COM5
"""
import argparse
import re
import time

class PrepareError(RuntimeError):
    pass

def send(port, command):
    # 丢弃旧提示符/READY，后续必须读取本次请求的新回复。
    port.reset_input_buffer()
    data=(command+'\r').encode('ascii')
    if port.write(data)!=len(data):
        raise PrepareError('串口命令未完整写出')
    port.flush()

def wait_for(port, timeout, predicate, emit):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        raw=port.readline()
        if not raw:
            continue
        line=raw.decode('utf-8',errors='replace').strip()
        emit(line)
        if any(x in line for x in ('command error=', 'STOP TEST', 'INIT FAILED', 'MP START FAILED',
                                  'startup failed', 'request=-', 'rejected',
                                  'no reboot', 'startup blocked', 'MP blocked')):
            raise PrepareError('设备拒绝切换或初始化失败，停止测试')
        value=predicate(line)
        if value is not None:
            return value
    raise PrepareError('等待设备回复超时；确认已进Recovery、波特率1500000及固件支持MP=STATUS')

def query(port, timeout, emit):
    send(port,'MP=STATUS')
    def parse(line):
        if not line.startswith('[REC_MP_STATUS] '):
            return None
        fields=dict(re.findall(r'(\w+)=([^\s]+)',line))
        required={'phase','ready','slot','fw1_valid','cpu_hz','mode','driver','promisc','ota_busy'}
        if not required.issubset(fields):
            raise PrepareError('状态回复不完整')
        return fields
    return wait_for(port,timeout,parse,emit)

def require_ready(state):
    expected={'phase':'MP','ready':'1','slot':'2','fw1_valid':'0','cpu_hz':'300000000',
              'mode':'1','driver':'1/0','promisc':'0','ota_busy':'0'}
    if any(state.get(k)!=v for k,v in expected.items()):
        raise PrepareError('当前状态不符合单STA MP要求，不能开始校准')
    try:
        valid = int(state['mp_mode']) == 1 and int(state['fw_state'], 0) & 0x10000 and int(state['test_state']) != 0
    except (KeyError, ValueError):
        valid = False
    if not valid:
        raise PrepareError('原厂MP初始化状态缺失或未完成，不能开始校准')

def prepare(port, timeout=20, emit=print):
    state=query(port,timeout,emit)
    if state['phase']=='MP':
        require_ready(state)
        return
    if state['phase']!='OTA' or state['slot']!='2' or state['fw1_valid']!='0' or state['ota_busy']!='0':
        raise PrepareError('需要空闲Recovery OTA；请先OTARECOVER，OTA事务开始后需重启')
    send(port,'MP=4')
    wait_for(port,timeout,lambda line: True if line.startswith('[REC_MP] READY;') else None,emit)
    # READY之后再次查询，避免把启动提示符当成成功。
    require_ready(query(port,timeout,emit))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True,help='COM5 或 /dev/ttyUSB0')
    parser.add_argument('--timeout',type=float,default=20)
    args=parser.parse_args()
    if args.timeout<=0:
        parser.error('timeout必须大于0')
    try:
        import serial
    except ImportError:
        parser.error('请先执行 python -m pip install pyserial')
    try:
        # 在打开端口前请求DTR/RTS为低，减少串口适配器触发板级复位的机会。
        # OS/适配器仍可能产生瞬态；现场必须确认WT接管时不会再次复位DUT。
        port=serial.Serial(port=None,baudrate=1500000,timeout=0.2,write_timeout=2)
        port.dtr=False
        port.rts=False
        port.port=args.port
        port.open()
        with port:
            prepare(port,args.timeout)
    except (PrepareError,serial.SerialException,OSError) as exc:
        print('FAIL:',exc)
        return 1
    print('MP环境已就绪，串口已释放。现在运行WT测试；不要复位或断电DUT。')
    print('固件已完成mp_start；本工具未执行射频校准，后续命令失败必须中止WT测试。')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
