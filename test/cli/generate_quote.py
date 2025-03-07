import requests
import json
import argparse
import base64
import time
import random
import hmac
from hashlib import sha256
from collections import OrderedDict
import urllib.parse

import _utils_

def get_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', type=str, help='the address of the ehsm_kms_server', required=True)
    parser.add_argument('--quote', type=str, help='the quote file want to generate', required=True)
    args = parser.parse_args()

    base_url = args.url + "/ehsm?Action="
    return base_url, args.quote

def generate_quote(base_url):
    payload = OrderedDict()
    payload["challenge"] = str(base64.b64encode("challenge123456".encode("utf-8")),'utf-8')
    params = _utils_.init_params(payload)
    print('generate_quote req:\n%s\n' %(params))
    GenerateQuote_resp = requests.post(url=base_url + "GenerateQuote", data=json.dumps(params), headers=_utils_.headers, verify=_utils_.use_secure_cert)
    if(_utils_.check_result(GenerateQuote_resp, 'GenerateQuote') == False):
        return
    print('generate_quote resp:\n%s\n' %(GenerateQuote_resp.text))
    return json.loads(resp.text)['result']['quote']

def generate_quote_with_file(base_url, quote_file):
    payload = OrderedDict()
    payload["challenge"] = str(base64.b64encode("challenge123456".encode("utf-8")),'utf-8')
    params = _utils_.init_params(payload)
    print('generate_quote req:\n%s\n' %(params))
    GenerateQuote_resp = requests.post(url=base_url + "GenerateQuote", data=json.dumps(params), headers=_utils_.headers, verify=_utils_.use_secure_cert)
    if(_utils_.check_result(GenerateQuote_resp, 'GenerateQuote') == False):
        return
    print('generate_quote resp:\n%s\n' %(GenerateQuote_resp.text))

    f = open(quote_file, "w")
    f.write(json.loads(GenerateQuote_resp.text)['result']['quote'])

if __name__ == "__main__":
    headers = _utils_.headers

    base_url, quote = get_args()

    generate_quote_with_file(base_url, quote)

