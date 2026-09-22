#!/bin/python3
# -*- coding: utf-8 -*-

import logging
import json
import httpx
import logging
import warnings
import asyncio
from functools import partial

log = logging.getLogger(__name__)
requests = httpx.AsyncClient()
warnings.simplefilter("ignore", UserWarning)

class TuGraphRestClient:

    def __init__(self, host, username, password):
        self.http_headers = {"Content-Type": "application/json", "Accept": "application/json"}
        self.host = host
        self.username = username
        self.password = password
        self._sync(self.__login__)


    def _sync(self, func):
        warnings.simplefilter("ignore", DeprecationWarning)
        return asyncio.get_event_loop().run_until_complete(func())


    def logout(self):
        r = self._sync(partial(self.__post__, 'logout'))
        data = r.get('data', r) if isinstance(r, dict) else r
        if isinstance(data, str):
            if data == '':
                return True
            else:
                return False
        # Flat success responses (e.g. {}) also mean the session ended.
        return True

    def refresh_token(self):
        r = self._sync(partial(self.__post__, 'refresh'))
        data = r.get('data', r) if isinstance(r, dict) else r
        if isinstance(data, str):
            return None
        else:
            return data.get("authorization", data.get("jwt"))


    def call_cypher(self, graph, cypher, timeout=0):
        data = {"script": cypher, "graph": graph, "timeout": timeout}
        r = self._sync(partial(self.__post__, 'cypher', data))
        # Current servers answer flat {"result": ...}; older ones wrapped it
        # as {"data": {"result": ...}}.
        if isinstance(r, dict) and "result" in r:
            return r["result"]
        return r["data"]["result"]

    def delete_specified_files(self, file_name):
        data = {"fileName" : file_name, "flag" : '0'}
        r = self._sync(partial(self.__post__, 'clear_cache', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["result"]

    def delete_specified_user_files(self, user_name):
        data = {"userName" : user_name, "flag" : '1'}
        r = self._sync(partial(self.__post__, 'clear_cache', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["result"]

    def delete_all_user_files(self):
        data = {"flag" : '2'}
        r = self._sync(partial(self.__post__, 'clear_cache', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["result"]

    def upload_file(self, file_name):
        # fragment_size at most 1024 * 1024 bytes
        fragment_size = 1024*1024
        with open(file_name, "rb") as f:
            idx = 0;
            while (True):
                stream = f.read(fragment_size)
                if not stream:
                    break
                begin = fragment_size * idx
                end = len(stream)
                self.http_headers["File-Name"] = "" + file_name
                self.http_headers["Begin-Pos"] = "" + str(begin)
                self.http_headers["Size"] = "" + str(end)
                r = self._sync(partial(self.__post_binary__, 'upload_files', data=stream))
                idx += 1

    def import_data(self, graph, schema, delimiter, continue_on_error, skip_packages, task_id, flag):
        data = {"graph" : graph, "schema" : schema, "delimiter" : delimiter,
                "continueOnError" : continue_on_error, "skipPackages" : skip_packages,
                "taskId" : task_id, "flag" : flag}
        r = self._sync(partial(self.__post__, 'import_data', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["taskId"]

    def import_schema(self, graph, schema):
        data = {"graph":graph, "description":schema}
        r = self._sync(partial(self.__post__, 'import_schema', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["result"]

    def check_file_with_size(self, file_name, size):
        data = {"fileName" : file_name, "flag" : "2" , "fileSize" : str(size)}
        r = self._sync(partial(self.__post__, 'check_file', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]["pass"]

    def import_progress(self, task_id):
        data = {"taskId" : task_id}
        r = self._sync(partial(self.__post__, 'import_progress', data))
        if isinstance(r['data'], str):
            return None
        else:
            return r["data"]


    async def __login__(self):
        try:
            # The server requires `user` (not `userName`) and answers with a
            # flat object carrying `jwt`; older servers wrapped it as
            # data.authorization. Accept both shapes.
            j_data = {}
            j_data["user"] = self.username
            j_data["password"] = self.password
            r = await self.__post__('login', j_data)
            data = r.get('data', r)
            jwt = data.get('authorization', data.get('jwt'))
            if not jwt:
                raise IOError('login response carries no token: %s' % (r,))
            self.http_headers["Authorization"] = "Bearer " + jwt
        except Exception as e:
            raise IOError('Failed to login to server {}: {}'.format(self.host, e))

    async def __refresh__(self):
        try:
            r = await self.__post__('refresh')
            data = r.get('data', r)
            jwt = data.get('authorization', data.get('jwt'))
            if not jwt:
                raise IOError('refresh response carries no token: %s' % (r,))
            self.http_headers["Authorization"] = "Bearer " + jwt
        except Exception as e:
            raise IOError('Failed to login to server {}: {}'.format(self.host, e))


    async def __post__(self, relative_url, data_dict=None):
        get_func = partial(
                requests.post,
                url=self.host + relative_url,
                headers=self.http_headers,
                json=data_dict,
                timeout=None
        )
        r = await self.__get_result__(get_func)
        if (r[0]):
            return r[1]
        else:
            # Current servers report `error_message`; older ones `errorMessage`.
            err = r[1].get("errorMessage", r[1].get("error_message", r[1]))
            return {"data":{"result":err}}


    async def __post_binary__(self, relative_url, data=None):
        get_func = partial(
            requests.post,
            url=self.host + relative_url,
            headers=self.http_headers,
            content=data,
            timeout=None
        )
        r = await self.__get_result__(get_func)
        if (r[0]):
            return r[1]
        else:
            return {"data":{"result":r[1]["errorMessage"]}}


    async def __get_result__(self, get_func):
        r = await get_func()
        js = {}
        try:
            js = json.loads(r.text or "{}")
        except Exception as e:
            logging.error(e)
        if r.status_code == 200:
            # Current servers answer flat JSON with no error envelope; older
            # ones carried errorCode == "200" on success.
            if js.get("errorCode", "200") == "200":
                return (True, js)
            else:
                return (False, js)