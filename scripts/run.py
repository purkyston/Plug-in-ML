#!python3
"""Run RPSCC system."""
# coding : utf-8

import subprocess
import time

from threading import Thread

def run(prog):
    """Run shell subprocess."""
    subprocess.check_call(prog, shell=True)

if __name__ == '__main__':

    prefix = 'ssh -o StrictHostKeyChecking=no '

    node_list = ['162.105.146.128', 'ip6-server14', 'ip6-server13']

    for node in node_list:
        print('clearing node: ' + node)
        prog = 'cd rpscc_deploy/scripts/; python3 kill_all.py'
        prog = prefix + 'zhengpeikai@' + node + ' \'' + prog + '\''
        prog_thread = Thread(target=run, args=(prog, ))
        prog_thread.deamon = True
        prog_thread.start()

    for node in node_list:
        print('copy scripts to node: ' + node)
        prog = 'scp ../src/channel/linear_regression.py zhengpeikai@' + node + ':rpscc_deploy/src/channel/'
        prog_thread = Thread(target=run, args=(prog, ))
        prog_thread.deamon = True
        prog_thread.start()

    time.sleep(5)

    worker_num = 3

    master_prog = ' cd rpscc_deploy/build/src/master; ./master_main --worker_num=' + str(worker_num) + ' --server_num=1 --listen_port=16666 --master_ip_port=162.105.146.128:16666 --key_range=100 --bound=1'
    master_prog = prefix + 'zhengpeikai@162.105.146.128' + ' \'' + master_prog + '\''
    master_thread = Thread(target=run, args=(master_prog, ))
    master_thread.deamon = True
    master_thread.start()

    time.sleep(5)

    server_prog = ' cd rpscc_deploy/build/src/server; ./server_main --master_ip_port=162.105.146.128:16666 --net_interface=eno1 --server_port=17777'
    server_prog = prefix + 'zhengpeikai@162.105.146.128' + ' \'' + server_prog + '\''
    server_thread = Thread(target=run, args=(server_prog, ))
    server_thread.deamon = True
    server_thread.start()

    time.sleep(5)

    agent_list = ['162.105.146.128', 'ip6-server14', 'ip6-server13']
    net_interface = ['eno1', 'eno2', 'eno2']
    worker_cnt = 0
    for ip, interface in zip(agent_list, net_interface):
        agent_prog = ' cd rpscc_deploy/build/src/agent; ./agent_main --net_interface=' + interface + ' --listen_port=15555 --master_ip_port=162.105.146.128:16666'
        agent_prog = prefix + 'zhengpeikai@' + ip + ' \'' + agent_prog + ' \''
        agent_thread = Thread(target=run, args=(agent_prog, ))
        agent_thread.deamon = True
        agent_thread.start()
        worker_cnt += 1
        if worker_cnt >= worker_num:
            break

    time.sleep(5)

    worker_list = agent_list
    worker_cnt = 0
    for ip in worker_list:
        worker_prog = ' cd rpscc_deploy/src/channel; python3 linear_regression.py distributed'
        worker_prog = prefix + 'zhengpeikai@' + ip + ' \'' + worker_prog + '\''
        worker_thread = Thread(target=run, args=(worker_prog, ))
        worker_thread.deamon = True
        worker_thread.start()
        worker_cnt += 1
        if worker_cnt >= worker_num:
            break