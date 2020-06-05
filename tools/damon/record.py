#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Record data access patterns of the target process.
"""

import argparse
import copy
import os
import signal
import subprocess
import time

debugfs_attrs = None
debugfs_record = None
debugfs_pids = None
debugfs_monitor_on = None

def set_target_pid(pid):
    return subprocess.call('echo %s > %s' % (pid, debugfs_pids), shell=True,
            executable='/bin/bash')

def turn_damon(on_off):
    return subprocess.call("echo %s > %s" % (on_off, debugfs_monitor_on),
            shell=True, executable="/bin/bash")

def is_damon_running():
    with open(debugfs_monitor_on, 'r') as f:
        return f.read().strip() == 'on'

def do_record(target, is_target_cmd, attrs, old_attrs):
    if os.path.isfile(attrs.rfile_path):
        os.rename(attrs.rfile_path, attrs.rfile_path + '.old')

    if attrs.apply():
        print('attributes (%s) failed to be applied' % attrs)
        cleanup_exit(old_attrs, -1)
    print('# damon attrs: %s' % attrs)
    if is_target_cmd:
        p = subprocess.Popen(target, shell=True, executable='/bin/bash')
        target = p.pid
    if set_target_pid(target):
        print('pid setting (%s) failed' % target)
        cleanup_exit(old_attrs, -2)
    if turn_damon('on'):
        print('could not turn on damon' % target)
        cleanup_exit(old_attrs, -3)
    while not is_damon_running():
        time.sleep(1)
    print('Press Ctrl+C to stop')
    if is_target_cmd:
        p.wait()
    while True:
        # damon will turn it off by itself if the target tasks are terminated.
        if not is_damon_running():
            break
        time.sleep(1)

    cleanup_exit(old_attrs, 0)

class Attrs:
    sample_interval = None
    aggr_interval = None
    regions_update_interval = None
    min_nr_regions = None
    max_nr_regions = None
    rbuf_len = None
    rfile_path = None

    def __init__(self, s, a, r, n, x, l, f):
        self.sample_interval = s
        self.aggr_interval = a
        self.regions_update_interval = r
        self.min_nr_regions = n
        self.max_nr_regions = x
        self.rbuf_len = l
        self.rfile_path = f

    def __str__(self):
        return "%s %s %s %s %s %s %s" % (self.sample_interval, self.aggr_interval,
                self.regions_update_interval, self.min_nr_regions,
                self.max_nr_regions, self.rbuf_len, self.rfile_path)

    def attr_str(self):
        return "%s %s %s %s %s " % (self.sample_interval, self.aggr_interval,
                self.regions_update_interval, self.min_nr_regions,
                self.max_nr_regions)

    def record_str(self):
        return '%s %s ' % (self.rbuf_len, self.rfile_path)

    def apply(self):
        ret = subprocess.call('echo %s > %s' % (self.attr_str(), debugfs_attrs),
                shell=True, executable='/bin/bash')
        if ret:
            return ret
        return subprocess.call('echo %s > %s' % (self.record_str(),
            debugfs_record), shell=True, executable='/bin/bash')

def current_attrs():
    with open(debugfs_attrs, 'r') as f:
        attrs = f.read().split()
    attrs = [int(x) for x in attrs]

    with open(debugfs_record, 'r') as f:
        rattrs = f.read().split()
    attrs.append(int(rattrs[0]))
    attrs.append(rattrs[1])
    return Attrs(*attrs)

def cmd_args_to_attrs(args):
    "Generate attributes with specified arguments"
    sample_interval = args.sample
    aggr_interval = args.aggr
    regions_update_interval = args.updr
    min_nr_regions = args.minr
    max_nr_regions = args.maxr
    rbuf_len = args.rbuf
    if not os.path.isabs(args.out):
        args.out = os.path.join(os.getcwd(), args.out)
    rfile_path = args.out
    return Attrs(sample_interval, aggr_interval, regions_update_interval,
            min_nr_regions, max_nr_regions, rbuf_len, rfile_path)

def cleanup_exit(orig_attrs, exit_code):
    if is_damon_running():
        if turn_damon('off'):
            print('failed to turn damon off!')
        while is_damon_running():
            time.sleep(1)
    if orig_attrs:
        if orig_attrs.apply():
            print('original attributes (%s) restoration failed!' % orig_attrs)
    exit(exit_code)

def sighandler(signum, frame):
    print('\nsignal %s received' % signum)
    cleanup_exit(orig_attrs, signum)

def chk_update_debugfs(debugfs):
    global debugfs_attrs
    global debugfs_record
    global debugfs_pids
    global debugfs_monitor_on

    debugfs_damon = os.path.join(debugfs, 'damon')
    debugfs_attrs = os.path.join(debugfs_damon, 'attrs')
    debugfs_record = os.path.join(debugfs_damon, 'record')
    debugfs_pids = os.path.join(debugfs_damon, 'pids')
    debugfs_monitor_on = os.path.join(debugfs_damon, 'monitor_on')

    if not os.path.isdir(debugfs_damon):
        print("damon debugfs dir (%s) not found", debugfs_damon)
        exit(1)

    for f in [debugfs_attrs, debugfs_record, debugfs_pids, debugfs_monitor_on]:
        if not os.path.isfile(f):
            print("damon debugfs file (%s) not found" % f)
            exit(1)

def chk_permission():
    if os.geteuid() != 0:
        print("Run as root")
        exit(1)

def set_argparser(parser):
    parser.add_argument('target', type=str, metavar='<target>',
            help='the target command or the pid to record')
    parser.add_argument('-s', '--sample', metavar='<interval>', type=int,
            default=5000, help='sampling interval')
    parser.add_argument('-a', '--aggr', metavar='<interval>', type=int,
            default=100000, help='aggregate interval')
    parser.add_argument('-u', '--updr', metavar='<interval>', type=int,
            default=1000000, help='regions update interval')
    parser.add_argument('-n', '--minr', metavar='<# regions>', type=int,
            default=10, help='minimal number of regions')
    parser.add_argument('-m', '--maxr', metavar='<# regions>', type=int,
            default=1000, help='maximum number of regions')
    parser.add_argument('-l', '--rbuf', metavar='<len>', type=int,
            default=1024*1024, help='length of record result buffer')
    parser.add_argument('-o', '--out', metavar='<file path>', type=str,
            default='damon.data', help='output file path')
    parser.add_argument('-d', '--debugfs', metavar='<debugfs>', type=str,
            default='/sys/kernel/debug', help='debugfs mounted path')

def main(args=None):
    global orig_attrs
    if not args:
        parser = argparse.ArgumentParser()
        set_argparser(parser)
        args = parser.parse_args()

    chk_permission()
    chk_update_debugfs(args.debugfs)

    signal.signal(signal.SIGINT, sighandler)
    signal.signal(signal.SIGTERM, sighandler)
    orig_attrs = current_attrs()

    new_attrs = cmd_args_to_attrs(args)
    target = args.target

    target_fields = target.split()
    if not subprocess.call('which %s > /dev/null' % target_fields[0],
            shell=True, executable='/bin/bash'):
        do_record(target, True, new_attrs, orig_attrs)
    else:
        try:
            pid = int(target)
        except:
            print('target \'%s\' is neither a command, nor a pid' % target)
            exit(1)
        do_record(target, False, new_attrs, orig_attrs)

if __name__ == '__main__':
    main()
