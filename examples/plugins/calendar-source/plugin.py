"""Developer example: read real calendar records from a user-configured HTTPS JSON endpoint."""

import json
from urllib.parse import urlencode, urlsplit
from urllib.request import urlopen

from asterion_plugin_sdk import serve


def dispatch(method, params):
    if method == 'view':
        return [{'contract': 'data.provider + ui.table', 'runtime': 'local Python code'}]
    if method == 'plan':
        request = params['request']
        return [{'api': 'calendar', 'params': {'start': request['start'], 'end': request['end'], 'exchange': request['exchange']},
                 'start': request['start'], 'end': request['end'],
                 'fields': ['exchange', 'date', 'is_open', 'previous_trading_day'], 'limit': 4000}]
    if method in {'probe', 'fetch'}:
        endpoint = params['configuration']['endpoint']
        if urlsplit(endpoint).scheme != 'https':
            raise ValueError('HTTPS is required')
        query = params['partition']['params'] if method == 'fetch' else {'probe': '1'}
        with urlopen(endpoint + ('&' if '?' in endpoint else '?') + urlencode(query), timeout=20) as response:
            content = response.read(7_500_001)
        if len(content) > 7_500_000:
            raise ValueError('Response too large')
        rows = json.loads(content)
        if not isinstance(rows, list):
            raise ValueError('Expected an array of calendar records')
        return 'connected' if method == 'probe' else rows
    if method == 'normalize':
        return params['rows']
    raise ValueError('Unknown method')


serve(dispatch)
