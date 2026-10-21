#!/usr/bin/env python3
"""Writes recorded.h from SOAP traces of vcsim and tools/fake-esxi.

Run the end-to-end tests (or casement-sim) against both, started with
tracing (vcsim -trace -trace-file vcsim.log, fake-esxi -trace fake.log),
then:

    python3 tests/unit/record_replies.py vcsim.log fake.log > tests/unit/recorded.h
"""

import re
import sys


def exchanges(path):
    """The (request, response) pairs of a trace, in order."""
    with open(path) as f:
        parts = re.split(r'^(Request|Response): ', f.read(), flags=re.M)
    # Text before the first marker, then marker and message in turn.
    messages = [(parts[i], parts[i + 1].strip()) for i in range(1, len(parts), 2)]
    pairs = []
    for i in range(0, len(messages) - 1, 2):
        assert messages[i][0] == 'Request' and messages[i + 1][0] == 'Response'
        pairs.append((messages[i][1], messages[i + 1][1]))
    return pairs


def method(request):
    m = re.search(r'Body>\s*<(\w+)', request)
    return m.group(1) if m else '?'


def find(pairs, name, match=lambda request, response: True, nth=0):
    """The response to the nth call of `name` that `match` accepts."""
    return [r for q, r in pairs if method(q) == name and match(q, r)][nth]


def main():
    vcsim = exchanges(sys.argv[1])
    fake = exchanges(sys.argv[2])
    items = [
        ('service_content', find(vcsim, 'RetrieveServiceContent')),
        ('login', find(vcsim, 'Login', lambda q, r: 'Fault' not in r)),
        ('login_invalid', find(vcsim, 'Login', lambda q, r: 'InvalidLogin' in r)),
        ('container_view', find(vcsim, 'CreateContainerView')),
        ('vms_in_view', find(vcsim, 'RetrievePropertiesEx', lambda q, r: 'ContainerView' in q)),
        ('destroy_view', find(vcsim, 'DestroyView')),
        ('path_found', find(vcsim, 'FindByInventoryPath', lambda q, r: 'type="VirtualMachine"' in r)),
        ('path_missing', find(vcsim, 'FindByInventoryPath', lambda q, r: 'returnval' not in r)),
        ('vm_properties',
         find(vcsim, 'RetrievePropertiesEx', lambda q, r: 'ContainerView' not in q and 'Fault' not in r)),
        ('object_not_found', find(vcsim, 'RetrievePropertiesEx', lambda q, r: 'ManagedObjectNotFound' in r)),
        ('no_acquire_ticket', find(vcsim, 'AcquireTicket')),
        ('logout', find(vcsim, 'Logout')),
        ('fake_vms_in_view',
         find(fake, 'RetrievePropertiesEx',
              lambda q, r: 'ContainerView' in q and 'web-01' in r and 'Fault' not in r, -1)),
        ('ticket_vcenter8', find(fake, 'AcquireTicket', lambda q, r: 'sslThumbprint' in r and '<url>' in r)),
        ('ticket_vsphere9', find(fake, 'AcquireTicket', lambda q, r: 'sslCertificate' in r)),
        ('no_permission', find(fake, 'AcquireTicket', lambda q, r: 'NoPermission' in r)),
    ]
    print("// SOAP replies recorded from govmomi's vcsim (v0.56.0) and tools/fake-esxi")
    print('// (the same simulator, with console tickets) by tests/unit/record_replies.py.')
    print()
    print('#pragma once')
    print()
    print('#include <string_view>')
    print()
    print('namespace recorded {')
    print()
    for name, body in items:
        assert ')xml"' not in body
        print(f'constexpr std::string_view {name} = R"xml({body})xml";')
        print()
    print('}  // namespace recorded')


if __name__ == '__main__':
    main()
