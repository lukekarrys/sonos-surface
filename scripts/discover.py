#!/usr/bin/env python3
"""Read-only SSDP discovery and independent playback-state probe. Never mutates Sonos."""
import argparse
import json
import socket
import time
import urllib.request
import xml.etree.ElementTree as ET


def field(xml, name):
    root = ET.fromstring(xml)
    return next((e.text or '' for e in root.iter() if e.tag.split('}')[-1] == name), '')


def probe(ip):
    socket.inet_aton(ip)
    base = 'http://' + ip + ':1400'
    with urllib.request.urlopen(base + '/xml/device_description.xml', timeout=3) as reply:
        description = reply.read(65536)
    result = {'sonos_ip': ip, 'sonos_uid': field(description, 'UDN').removeprefix('uuid:'),
              'room': field(description, 'roomName')}
    action = 'GetTransportInfo'
    urn = 'urn:schemas-upnp-org:service:AVTransport:1'
    body = ('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            f'<u:{action} xmlns:u="{urn}"><InstanceID>0</InstanceID></u:{action}>'
            '</s:Body></s:Envelope>').encode()
    request = urllib.request.Request(base + '/MediaRenderer/AVTransport/Control', data=body,
        headers={'Content-Type': 'text/xml; charset="utf-8"', 'SOAPACTION': f'"{urn}#{action}"'})
    with urllib.request.urlopen(request, timeout=5) as reply:
        result['playback'] = field(reply.read(65536), 'CurrentTransportState')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip', help='Probe one IPv4 speaker directly if multicast is blocked')
    args = parser.parse_args()
    ips = set()
    if args.ip:
        ips.add(args.ip)
    else:
        request = ('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n'
                   'MAN: "ssdp:discover"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n')
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(0.5)
            sock.sendto(request.encode(), ('239.255.255.250', 1900))
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                try:
                    data, address = sock.recvfrom(8192)
                    if b'zoneplayer' in data.lower():
                        ips.add(address[0])
                except socket.timeout:
                    pass
    for ip in sorted(ips):
        try:
            print(json.dumps(probe(ip)))
        except Exception as error:
            print(json.dumps({'sonos_ip': ip, 'error': str(error)}))
    if not ips:
        print('No SSDP replies. Check LAN permissions/VLAN or use --ip SPEAKER_IP.')
