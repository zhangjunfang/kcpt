#include "wasclient.h"
#include <netutil.h>
#include <tools.h>
#include <BaseTools.h>

#include <fstream>
using std::ifstream;

#include "../tools/utils.h"
#include "jt808prot.h"
#include "urlcoding.h"

WasClient::WasClient()
{
	_hostfile = "";
	_thread_num = 8;
	_max_timeout = 180;
}

WasClient::~WasClient()
{
	Stop();
}

bool WasClient::Init(ISystemEnv *pEnv)
{
	_pEnv = pEnv ;

	char szbuf[512] = {0};

	if ( ! pEnv->GetString("hostfile", szbuf)) {
		OUT_ERROR( NULL, 0, NULL, "get media_path  failure" ) ;
		return false;
	}
	_hostfile = szbuf;

	int nvalue = 0 ;
	if (pEnv->GetInteger("client_thread", nvalue)) {
		_thread_num = nvalue;
	}

	if ( pEnv->GetInteger( "was_user_time" , nvalue ) ) {
		_max_timeout = nvalue ;
	}

	if(!loadHost()) {
		OUT_ERROR( NULL, 0, NULL, "load host info fail" ) ;
		return false;
	}

	setpackspliter( &_spliter808 ) ;

	_uniqResp.init(10);

	return true;
}

bool WasClient::Start()
{
	return StartClient( "" , 0 , _thread_num, _max_timeout) ;
}

void WasClient::Stop()
{
	StopClient() ;
}

void WasClient::TimeWork()
{
	while ( Check() ) {
		sleep(60) ;

		registerTerm();
	}
}

void WasClient::NoopWork()
{
	while( Check() ) {
		sleep(30);

		HandleOfflineUsers();

		list<string> keys;
		_uniqResp.check(keys);
	}
}

void WasClient::on_data_arrived(socket_t *sock, const void *data, int len)
{
	const char *ptr = (const char*)data;
	const char *ip    = sock->_szIp;
	unsigned int port = sock->_port;
	OUT_HEX(ip, port, "RECV", ptr, len);

	const size_t min_len = sizeof(JTHeader) + sizeof(JTFooter);
	const size_t max_len = (min_len + sizeof(MsgPart) + 0x3ff) * 2;
	if(len < (int)min_len || len > (int)max_len) {
		OUT_WARNING(ip, port, "RECV", "invalid message, drop it! length %d", len) ;
		return;
	}

	vector<unsigned char> dstBuf;
	unsigned char *dstPtr;
	size_t dstLen;

	dstBuf.resize(len);
	dstPtr = &dstBuf[0];
	dstLen = Utils::deCode808((uint8_t*) data, len, dstPtr);

	JTHeader *header = (JTHeader*)dstPtr;
	uint16_t msgid = htons(header->msgid);
	uint16_t flags = htons(header->length);

	uint16_t ispack = flags & 0x2000; //第13位是否分包标识
	uint16_t length = flags & 0x3ff;  //第0 ~ 9位消息体长度
	if(ispack != 0 && dstLen != length + min_len + sizeof(MsgPart)) {
		OUT_WARNING(ip, port, "RECV", "invalid message, drop it! flags %x, length %u, %u", flags, dstLen, length);
		return;
	} else if(ispack == 0 && dstLen != length + min_len) {
		OUT_WARNING(ip, port, "RECV", "invalid message, drop it! flags %x, length %u, %u", flags, dstLen, length);
		return;
	}

	string simid;
	if(!bcd2sim(header->simid, simid)) {
		OUT_WARNING(ip, port, "RECV", "invalid message, drop it! length %d", dstLen) ;
		return;
	}

	string keyStr;
	string valStr;

	User user = _online_user.GetUserBySocket(sock);
	if(user._user_id.empty()) {
		return;
	}
	user._last_active_time = time(NULL);
	_online_user.SetUser(user._user_id, user);

	if(msgid == 0x8001) {
		if (dstLen != sizeof(CommonResp)) {
			return;
		}

		CommonResp *rsp = (CommonResp*)dstPtr;
		uint16_t rmsgid = htons(rsp->resp_msg_id);
		uint16_t rseqid = htons(rsp->resp_seq);
		uint8_t result = rsp->result;


		if(rmsgid == 0x0102 && result == 0) {
			user._user_state = User::ON_LINE;
			_online_user.SetUser(user._user_id, user);

			share::Guard guard(_mutex);
			map<string, vector<uint8_t> >::iterator it = _cache.find(user._user_id);
			if (it != _cache.end()) {
				SendData(sock, (char*)&it->second[0], it->second.size());
				OUT_HEX(ip, port, "SEND", (char*)&it->second[0], it->second.size());
				_cache.erase(it);
			}

			return;
		} else if(rmsgid == 0x0102 && result != 0) {
			CloseSocket(user._fd);
			return;
		}

		char keybuf[128];
		snprintf(keybuf, 128, "%s_%u_%u", simid.c_str(), rmsgid, rseqid);
		if(_uniqResp.exist(keybuf)) {
			return; //避免多个前置机回复同样的应答给终端
		}
		_uniqResp.insert(keybuf);
	} else if(msgid == 0x8100) {
		_online_user.DeleteUser(sock);
		CloseSocket(user._fd);

		TermRegisterResp *rsp = (TermRegisterResp*)dstPtr;
		if(rsp->result == 0) { //注册成功往redis保存鉴权码
			string akey((char*)rsp->akey, length - 3);

			UrlCoding urlcoding('?');
			akey = urlcoding.encode(akey);

			_pEnv->GetRedisCache()->HSet("KCTX.FORWARD808.AKEY", user._user_id.c_str(), akey.c_str());
		}

		return;
	}

	_pEnv->GetWasServer()->HandleData(simid, (uint8_t*)data, len);
}

void WasClient::on_dis_connection(socket_t *sock)
{
	User user = _online_user.GetUserBySocket(sock);
	if(user._user_id.empty()) {
		OUT_INFO( sock->_szIp , sock->_port , "NONE", "ENV on_dis_connection fd %d", sock->_fd ) ;
		return;
	}

	//专门处理底层的链路突然断开的情况，不处理超时和正常流程下的断开情况。
	OUT_INFO( sock->_szIp , sock->_port , user._user_id.c_str(), "ENV on_dis_connection fd %d", sock->_fd ) ;

	user._fd = NULL;
	user._user_state  = User::OFF_LINE ;
	_online_user.SetUser(user._user_id, user);
}

int WasClient::build_login_msg( User &user, char *buf,int buf_len )
{
	vector<uint8_t> msg;

	if(!buildTerm0102(user._user_id, msg) || (int)msg.size() > buf_len) {
		memcpy(buf, "\x7e\x00\x7e", 3);
		return 3;
	}

	memcpy(buf, &msg[0], msg.size());
	OUT_HEX(user._ip.c_str(), user._port, "SEND", (char*)&msg[0], msg.size());

	return msg.size();
}

void WasClient::HandleOfflineUsers()
{
	vector<User> users = _online_user.GetOfflineUsers(_max_timeout);

	for(int i = 0; i < (int)users.size(); i++) {
		User &user = users[i];

		if(!_pEnv->GetWasServer()->ChkTerminal(user._user_name)) {
			OUT_INFO(user._ip.c_str(), user._port, "TOUT", "%s timeout and real term is offline", user._user_id.c_str());
			continue;
		}
		OUT_INFO(user._ip.c_str(), user._port, "TOUT", "%s timeout and retry connect", user._user_id.c_str());

		user._user_state = User::OFF_LINE;
		ConnectServer(user, 1);
		_online_user.AddUser( user._user_id, user );
	}
}

bool WasClient::bcd2sim(const uint8_t *bcd, string &sim)
{
    const char *TAB = "0123456789abcdef";
    const size_t BCDLEN = 6;
    char buf[BCDLEN * 2 + 1];
    size_t i;
    size_t begin = string::npos;
    size_t end = 0;

    for(i = 0; i < BCDLEN; ++i) {
        buf[end] = TAB[bcd[i] >> 4];
        if(begin == string::npos && buf[end] != '0') {
            begin = end;
        }
        ++end;

        buf[end] = TAB[bcd[i] & 0xf];
        if(begin == string::npos && buf[end] != '0') {
            begin = end;
        }
        ++end;
    }
    buf[end] = 0;

    if(begin == string::npos) {
        return false;
    }
    sim = buf + begin;

    return true;
}

bool WasClient::sim2bcd(const string &sim, uint8_t *bcd)
{
    const size_t BCDLEN = 6;
    size_t i;
    size_t n;
    uint8_t h;
    uint8_t l;

    string tmp = "000000000000" + sim;
    string hex = tmp.substr(tmp.length() - BCDLEN * 2);

    n = 0;
    for(i = 0; i < BCDLEN; ++i) {
    	h = hex[n++] - '0';
    	h = (h > 9) ? 0 : h;
    	l = hex[n++] - '0';
    	l = (l > 9) ? 0 : l;

    	bcd[i] = ((h << 4) | l);
    }

    return true;
}

uint8_t WasClient::get_check_sum(uint8_t *buf, size_t len)
{
	if(buf == NULL || len < 1)
		return 0;
	unsigned char check_sum = 0;
	for (int i = 0; i < (int) len; i++) {
		check_sum ^= buf[i];
	}
	return check_sum;
}

bool WasClient::buildTerm0100(const TermInfo &term, vector<uint8_t> &msg)
{
	TermRegister *reg;

	if(term.plate.length() > 64) {
		return false;
	}

	msg.resize(sizeof(TermRegister) + term.plate.length() + sizeof(JTFooter));
	memset(&msg[0], 0, msg.size());
	reg = (TermRegister*)&msg[0];

	reg->header.delim = 0x7e;
	reg->header.msgid = htons(0x0100);
	reg->header.length = htons(msg.size() - sizeof(JTHeader) - sizeof(JTFooter));
	sim2bcd(term.sim, reg->header.simid);
	reg->header.seqid = htons(0);

	term.termMfrs.copy(reg->corp_id, 5);
	term.termType.copy(reg->termtype, 20);
	term.termID.copy(reg->termid, 7);
	reg->carcolor = atoi(term.color.c_str());
	term.plate.copy(reg->carplate, term.plate.length());

	JTFooter *footer = (JTFooter*)(&msg[0] + sizeof(TermRegister) + term.plate.length());
	footer->check_sum = get_check_sum(&msg[1], msg.size() - 3);
	footer->delim = 0x7e;

	vector<unsigned char> msgBuf;
	unsigned char *msgPtr;
	size_t msgLen;

	msgBuf.resize(msg.size() * 2);
	msgPtr = &msgBuf[0];
	msgLen = Utils::enCode808(&msg[0], msg.size(), msgPtr);
	msg.resize(msgLen);
	memcpy(&msg[0], msgPtr, msgLen);

	return true;
}

bool WasClient::buildTerm0102(const string &userid, vector<uint8_t> &msg)
{
	size_t pos = userid.find('_');
	if(pos == string::npos) {
		return false;
	}
	string sim = userid.substr(0, pos);

	string akey;
	_pEnv->GetRedisCache()->HGet("KCTX.FORWARD808.AKEY", userid.c_str(), akey);
	if (akey.empty()) {
		return false; //redis 中不存在鉴权码
	}
	UrlCoding urlcoding('?');
	akey = urlcoding.decode(akey);

	JTHeader *header;
	JTFooter *footer;
	char *akeyptr;

	msg.resize(sizeof(JTHeader) + akey.length() + sizeof(JTFooter));
	header = (JTHeader*)&msg[0];
	footer = (JTFooter*)&msg[sizeof(JTHeader) + akey.length()];
	akeyptr = (char*)&msg[sizeof(JTHeader)];

	header->delim = 0x7e;
	header->msgid = htons(0x0102);
	header->length = htons(akey.length());
	sim2bcd(sim, header->simid);
	header->seqid = htons(0);
	akey.copy(akeyptr, akey.length());
	footer->check_sum = get_check_sum(&msg[1], msg.size() - 3);
	footer->delim = 0x7e;

	vector<unsigned char> msgBuf;
	unsigned char *msgPtr;
	size_t msgLen;

	msgBuf.resize(msg.size() * 2);
	msgPtr = &msgBuf[0];
	msgLen = Utils::enCode808(&msg[0], msg.size(), msgPtr);
	msg.resize(msgLen);
	memcpy(&msg[0], msgPtr, msgLen);

	return true;
}

void WasClient::registerTerm()
{
	string keyStr;
	string valStr;
	vector<string> fields;

	string userid;
	User user;

	vector<string> allInfo;
	vector<string> allAkey;
	vector<string>::iterator itVs;
	map<string, HostInfo>::iterator itMsh;
	set<string> akeyIdx;
	set<string>::iterator itSs;

	TermInfo term;
	vector<uint8_t> msg;

	allInfo.reserve(100000);
	allInfo.clear();
	_pEnv->GetRedisCache()->HKeys("KCTX.FORWARD808.INFO", allInfo);
	allAkey.reserve(allInfo.size() * _hostInfo.size());
	allAkey.clear();
	_pEnv->GetRedisCache()->HKeys("KCTX.FORWARD808.AKEY", allAkey);

	for(itVs = allAkey.begin(); itVs != allAkey.end(); ++itVs) {
		akeyIdx.insert(*itVs); //用于检索鉴权码是否存在
	}

	for(itVs = allInfo.begin(); itVs != allInfo.end(); ++itVs) {
		const string &sim = *itVs;

		msg.clear(); //开始每个终端注册前把消息容器清空
		for(itMsh = _hostInfo.begin(); itMsh != _hostInfo.end(); ++itMsh) {
			const string &code = itMsh->first;
			const HostInfo &host = itMsh->second;

			keyStr = sim + "_" + code;
			if(akeyIdx.find(keyStr) != akeyIdx.end()) {
				continue; //redis 中已存在鉴权码
			}

			if(msg.empty()) { //同个终端注册消息只需要构建一次
				valStr.clear();
				_pEnv->GetRedisCache()->HGet("KCTX.FORWARD808.INFO", sim.c_str(), valStr);
				fields.clear();
				if (Utils::splitStr(valStr, fields, ',') != 5) {
					break; //redis中的注册信息格式不对
				}
				term.sim = sim;
				term.color = fields[0];
				term.plate = fields[1];
				term.termMfrs = fields[2];
				term.termType = fields[3];
				term.termID = fields[4];

				if(!buildTerm0100(term, msg)) {
					break;
				}
			}

			user._user_id = keyStr;
			user._user_state = User::ON_LINE;
			user._ip = host.ip;
			user._port = host.port;
			user._last_active_time = time(NULL);
			user._connect_info.last_reconnect_time = 0;
			user._connect_info.timeval = 0;
			user._fd = _tcp_handle.connect_nonb(user._ip.c_str(), user._port, 1);
			if(user._fd == NULL) {
				continue; //连接对接平台前置机失败。
			}
			_online_user.AddUser(user._user_id, user);

			SendData(user._fd, (char*)&msg[0], msg.size());
			OUT_HEX(user._ip.c_str(), user._port, "SEND" , (char*)&msg[0], msg.size());
		}
	}
}

bool WasClient::loadHost()
{
	ifstream ifs;
	string line;
	vector<string> fields;

	string key;
	HostInfo val;

	ifs.open(_hostfile.c_str());
	while(getline(ifs, line)) {
		if(line.empty() || line[0] == '#') {
			continue;
		}

		fields.clear();
		if(Utils::splitStr(line, fields, ':') != 5) {
			continue;
		}

		if(fields[0] != "WASCLIENT") {
			continue;
		}

		//WASCLIENT:编号:ip地址:端口:是否开鉴权
		key = fields[1];
		val.ip = fields[2];
		val.port = atoi(fields[3].c_str());
		_hostInfo.insert(make_pair(key, val));
	}

	if(_hostInfo.empty()) {
		return false;
	}

	return true;
}

bool WasClient::HandleData(const string &sim, const uint8_t *ptr, size_t len)
{
	string userid;
	map<string, HostInfo>::iterator it;

	for (it = _hostInfo.begin(); it != _hostInfo.end(); ++it) {
		const string &key = it->first;

		userid = sim + "_" + key;
		User user = _online_user.GetUserByUserId(userid);
		if (user._user_id.empty()) {
			continue;
		}

		if(user._fd == NULL || user._user_state != User::ON_LINE) {
			//模拟终端不在线暂存数据
			share::Guard guard(_mutex);
			map<string, vector<uint8_t> >::iterator it = _cache.find(userid);
			if (it != _cache.end()) {
				if (it->second.size() > 1024) {
					continue;
				}

				it->second.insert(it->second.end(), ptr, ptr + len);
			} else {
				vector<uint8_t> cache;
				cache.assign(ptr, ptr + len);
				_cache.insert(make_pair(userid, cache));
			}

			continue;
		}

		SendData(user._fd, (char*)ptr, len);
		OUT_HEX(user._ip.c_str(), user._port, "SEND", (char*)ptr, len);
	}

	return true;
}

bool WasClient::AddTerminal(const string &sim)
{
	string userid;
	map<string, HostInfo>::iterator it;

	for (it = _hostInfo.begin(); it != _hostInfo.end(); ++it) {
		const string &key = it->first;
		const HostInfo &val = it->second;

		userid = sim + "_" + key;
		User user = _online_user.GetUserByUserId(userid);
		if (!user._user_id.empty()) {
			continue;
		}

		user._user_id = userid;
		user._user_name = sim;
		user._fd = NULL;
		user._user_state = User::OFF_LINE;
		user._ip = val.ip;
		user._port = val.port;
		user._connect_info.last_reconnect_time = 0;
		user._connect_info.timeval = 0;

		ConnectServer(user, 1);
		_online_user.AddUser(userid, user);

		OUT_INFO(user._ip.c_str(), user._port, userid.c_str(), "add simulator terminal");
	}

	return true;
}

bool WasClient::DelTerminal(const string &sim)
{
	string userid;
	map<string, HostInfo>::iterator it;

	for (it = _hostInfo.begin(); it != _hostInfo.end(); ++it) {
		const string &key = it->first;

		userid = sim + "_" + key;

		_mutex.lock();
		map<string, vector<uint8_t> >::iterator itMsv = _cache.find(userid);
		if (itMsv != _cache.end()) {
			_cache.erase(itMsv);
		}
		_mutex.unlock();

		User user = _online_user.DeleteUser(userid);
		if (user._user_id.empty()) {
			continue;
		}

		if(user._fd != NULL) {
			CloseSocket(user._fd);
		}

		OUT_INFO(user._ip.c_str(), user._port, userid.c_str(), "del simulator terminal");
	}

	return true;
}
