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
    result = {'ip': ip, 'sonos_uid': field(description, 'UDN').removeprefix('uuid:'),
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


def discover_addresses():
    ips = set()
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
    return ips


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args(argv)
    try:
        ips = discover_addresses()
    except OSError:
        print('SSDP discovery failed. Check LAN permissions and network connectivity.')
        return 1
    if not ips:
        print('No SSDP replies. Check LAN permissions and multicast connectivity.')
        return 1
    succeeded = False
    for ip in sorted(ips):
        try:
            print(json.dumps(probe(ip)))
            succeeded = True
        except Exception as error:
            print(json.dumps({'ip': ip, 'error': str(error)}))
    return 0 if succeeded else 1


if __name__ == '__main__':
    raise SystemExit(main())
