#include"qcaopenssl.h"

extern "C"
{
#include<sasl/sasl.h>
}

#include<qhostaddress.h>
#include<qstringlist.h>

#define SASL_BUFSIZE 8192

static bool client_init = false;
static bool server_init = false;

/*
  TODO:
    - security layer, and support fetching the security layer strength
    - clean up the whole need-params junk
    - clean the rest and check for memory leaks
*/

sasl_callback_t clientcbs[] =
{
	{ SASL_CB_GETREALM, NULL, NULL },
	{ SASL_CB_USER, NULL, NULL },
	{ SASL_CB_AUTHNAME, NULL, NULL },
	{ SASL_CB_PASS, NULL, NULL },
	{ SASL_CB_LIST_END, NULL, NULL },
};

sasl_callback_t servercbs[] =
{
	{ SASL_CB_LIST_END, NULL, NULL },
};

static QByteArray makeByteArray(const void *in, unsigned int len)
{
	QByteArray buf(len);
	memcpy(buf.data(), in, len);
	return buf;
}

static void clearNeedParams(QCA_SASLNeedParams *np)
{
	np->auth = false;
	np->user = false;
	np->pass = false;
	np->realm = false;
}

static QCA_SASLNeedParams interactToNeedParams(sasl_interact_t *need)
{
	QCA_SASLNeedParams np;
	clearNeedParams(&np);
	for(int n = 0; need[n].id != SASL_CB_LIST_END; ++n) {
		if(need[n].id == SASL_CB_AUTHNAME)
			np.auth = true;
		if(need[n].id == SASL_CB_USER)
			np.user = true;
		if(need[n].id == SASL_CB_PASS)
			np.pass = true;
		if(need[n].id == SASL_CB_GETREALM)
			np.realm = true;
	}
	return np;
}

static QString addrString(const QCA_SASLHostPort &hp)
{
	return (hp.addr.toString() + ';' + QString::number(hp.port));
}

static QString methodsToString(const QStringList &methods)
{
	QString list;
	bool first = true;
	for(QStringList::ConstIterator it = methods.begin(); it != methods.end(); ++it) {
		if(!first)
			list += ' ';
		else
			first = false;
		list += (*it);
	}
	return list;
}

static void setInteract(sasl_interact_t *need, unsigned int type, const QString &s)
{
	QCString cs = s.utf8();
	for(int n = 0; need[n].id != SASL_CB_LIST_END; ++n) {
		if(need[n].id == type) {
			int len = cs.length();
			char *p = (char *)malloc(len+1);
			memcpy(p, cs.data(), len);
			p[len] = 0;
			need[n].result = p;
			need[n].len = len;
		}
	}
}

class SASLContext : public QCA_SASLContext
{
public:
	// core props
	QString service, host;
	QString localAddr, remoteAddr;

	// security props
	int secflags;
	int ssf_min, ssf_max;

	// params
	QString auth, user, pass, realm;

	sasl_conn_t *con;
	sasl_interact_t *need;
	QStringList mechlist;
	bool servermode;

	SASLContext()
	{
		con = 0;
		need = 0;
		reset();
	}

	~SASLContext()
	{
		reset();
	}

	void reset()
	{
		if(con) {
			if(need) {
				// free any results we may have allocated
				for(int n = 0; need[n].id != SASL_CB_LIST_END; ++n) {
					if(need[n].result)
						free((void *)(need[n].result));
				}
			}
			sasl_dispose(&con);
			con = 0;
		}

		localAddr = "";
		remoteAddr = "";
		auth = QString::null;
		user = QString::null;
		pass = QString::null;
		realm = QString::null;
		mechlist.clear();
		secflags = 0;
		ssf_min = 0;
		ssf_max = 0;
	}

	void setCoreProps(const QString &_service, const QString &_host, QCA_SASLHostPort *la, QCA_SASLHostPort *ra)
	{
		service = _service;
		host = _host;
		localAddr = la ? addrString(*la) : "";
		remoteAddr = ra ? addrString(*ra) : "";
	}

	void setSecurityProps(bool noPlain, bool noActive, bool noDict, bool noAnon, bool reqForward, bool reqCreds, bool reqMutual, int ssfMin, int ssfMax)
	{
		int sf = 0;
		if(noPlain)
			sf |= SASL_SEC_NOPLAINTEXT;
		if(noActive)
			sf |= SASL_SEC_NOACTIVE;
		if(noDict)
			sf |= SASL_SEC_NODICTIONARY;
		if(noAnon)
			sf |= SASL_SEC_NOANONYMOUS;
		if(reqForward)
			sf |= SASL_SEC_FORWARD_SECRECY;
		if(reqCreds)
			sf |= SASL_SEC_PASS_CREDENTIALS;
		if(reqMutual)
			sf |= SASL_SEC_MUTUAL_AUTH;
		secflags = sf;
		ssf_min = ssfMin;
		ssf_max = ssfMax;
	}

	void setsecprops()
	{
		sasl_security_properties_t secprops;
		secprops.min_ssf = ssf_min;
		secprops.max_ssf = ssf_max;
		secprops.maxbufsize = SASL_BUFSIZE;
		secprops.property_names = NULL;
		secprops.property_values = NULL;
		secprops.security_flags = secflags;
		sasl_setprop(con, SASL_SEC_PROPS, &secprops);
	}

	void fillparams(QCA_SASLNeedParams *np)
	{
		if(np->auth && !auth.isNull()) {
			setInteract(need, SASL_CB_AUTHNAME, auth);
			np->auth = false;
		}
		if(np->user && !user.isNull()) {
			setInteract(need, SASL_CB_USER, user);
			np->user = false;
		}
		if(np->pass && !pass.isNull()) {
			setInteract(need, SASL_CB_PASS, pass);
			np->pass = false;
		}
		if(np->realm && !realm.isNull()) {
			setInteract(need, SASL_CB_GETREALM, realm);
			np->realm = false;
		}
	}

	bool clientStart(const QStringList &_mechlist)
	{
		int r = sasl_client_new(service.latin1(), host.latin1(), localAddr.isEmpty() ? 0 : localAddr.latin1(), remoteAddr.isEmpty() ? 0 : remoteAddr.latin1(), clientcbs, 0, &con);
		if(r != SASL_OK)
			return false;

		setsecprops();

		mechlist = _mechlist;
		servermode = false;
		return true;
	}

	bool serverStart(const QString &realm, QStringList *mechlist)
	{
		int r = sasl_server_new(service.latin1(), host.latin1(), realm.latin1(), localAddr.isEmpty() ? 0 : localAddr.latin1(), remoteAddr.isEmpty() ? 0 : remoteAddr.latin1(), servercbs, 0, &con);
		if(r != SASL_OK)
			return false;

		setsecprops();

		const char *ml;
		r = sasl_listmech(con, 0, NULL, " ", NULL, &ml, 0, 0);
		if(r != SASL_OK)
			return false;
		*mechlist = QStringList::split(' ', ml);
		servermode = true;
		return true;
	}

	int clientFirstStep(QString *mech, QByteArray **out, QCA_SASLNeedParams *np)
	{
		clearNeedParams(np);
		bool supportClientSendFirst = out ? true: false;

		const char *clientout, *m;
		unsigned int clientoutlen;

		need = 0;
		QString list = methodsToString(mechlist);
		int r;
		while(1) {
			if(supportClientSendFirst)
				r = sasl_client_start(con, list.latin1(), &need, &clientout, &clientoutlen, &m);
			else
				r = sasl_client_start(con, list.latin1(), &need, NULL, NULL, &m);
			if(r != SASL_INTERACT)
				break;

			*np = interactToNeedParams(need);
			fillparams(np);
			if(np->auth || np->user || np->pass || np->realm)
				return NeedParams;
		}
		if(r != SASL_OK && r != SASL_CONTINUE)
			return Error;

		*mech = m;
		if(supportClientSendFirst && clientout)
			*out = new QByteArray(makeByteArray(clientout, clientoutlen));
		else
			*out = 0;

		if(r == SASL_OK)
			return Success;
		else
			return Continue;
	}

	int serverFirstStep(const QString &mech, const QByteArray *in, QByteArray *out)
	{
		const char *clientin = 0;
		unsigned int clientinlen = 0;
		if(in) {
			clientin = in->data();
			clientinlen = in->size();
		}
		const char *serverout;
		unsigned int serveroutlen;
		int r = sasl_server_start(con, mech.latin1(), clientin, clientinlen, &serverout, &serveroutlen);
		if(r != SASL_OK && r != SASL_CONTINUE)
			return Error;
		*out = makeByteArray(serverout, serveroutlen);
		if(r == SASL_OK)
			return Success;
		else
			return Continue;
	}

	int clientNextStep(const QByteArray &in, QByteArray *out, QCA_SASLNeedParams *np)
	{
		clearNeedParams(np);
		const char *clientout;
		unsigned int clientoutlen;
		int r;
		while(1) {
			r = sasl_client_step(con, in.data(), in.size(), &need, &clientout, &clientoutlen);
			if(r != SASL_INTERACT)
				break;

			*np = interactToNeedParams(need);
			fillparams(np);
			if(np->auth || np->user || np->pass || np->realm)
				return NeedParams;
		}
		if(r != SASL_OK && r != SASL_CONTINUE)
			return Error;
		*out = makeByteArray(clientout, clientoutlen);
		if(r == SASL_OK)
			return Success;
		else
			return Continue;
	}

	int serverNextStep(const QByteArray &in, QByteArray *out)
	{
		const char *serverout;
		unsigned int serveroutlen;
		int r = sasl_server_step(con, in.data(), in.size(), &serverout, &serveroutlen);
		if(r != SASL_OK && r != SASL_CONTINUE)
			return Error;
		if(r == SASL_OK) {
			out->resize(0);
			return Success;
		}
		*out = makeByteArray(serverout, serveroutlen);
		return Continue;
	}

	void setAuthname(const QString &s)
	{
		auth = s;
		if(need)
			setInteract(need, SASL_CB_AUTHNAME, s);
	}

	void setUsername(const QString &s)
	{
		user = s;
		if(need)
			setInteract(need, SASL_CB_USER, s);
	}

	void setPassword(const QString &s)
	{
		pass = s;
		if(need)
			setInteract(need, SASL_CB_PASS, s);
	}

	void setRealm(const QString &s)
	{
		realm = s;
		if(need)
			setInteract(need, SASL_CB_GETREALM, s);
	}
};

class QCACyrusSASL : public QCAProvider
{
public:
	QCACyrusSASL()
	{
		if(!client_init) {
			sasl_client_init(NULL);
			client_init = true;
		}
		if(!server_init) {
			sasl_server_init(NULL, "qca");
			server_init = true;
		}
	}

	~QCACyrusSASL()
	{
		sasl_done();
		client_init = false;
		server_init = false;
	}

	int capabilities() const
	{
		return QCA::CAP_SASL;
	}

	void *context(int cap)
	{
		if(cap == QCA::CAP_SASL)
			return new SASLContext;
		return 0;
	}
};

#ifdef QCA_PLUGIN
QCAProvider *createProvider()
#else
QCAProvider *createProviderCyrusSASL()
#endif
{
	return (new QCACyrusSASL);
}

/*
	int security;
	int sasl_maxoutbuf;

	d->security = 0;
	d->sasl_maxoutbuf = 0;

void QSASL::handle_step(int code, const char *clientout, unsigned int len)
{
	if(code == SASL_OK) {
		fprintf(stderr, "QSASL: Success\n");
		const int *ssfp;
		int r = sasl_getprop(d->con, SASL_SSF, (const void **)&ssfp);
		if(r == SASL_OK)
			d->security = *ssfp;
		sasl_getprop(d->con, SASL_MAXOUTBUF, (const void **)&d->sasl_maxoutbuf);
		authenticated();
	}
}

int QSASL::tryWrite()
{
	if(!d->bs)
		return -1;

	// take a section of the write buffer
	int size = d->sasl_maxoutbuf;
	QByteArray a = takeWrite(size);

	if(d->security > 0) {
		const char *out;
		unsigned int len;
		int r = sasl_encode(d->con, a.data(), a.size(), &out, &len);
		if(r != SASL_OK) {
			error(ErrWrite);
			return -1;
		}
		QByteArray b(len);
		memcpy(b.data(), out, len);
		d->bs->write(b);
	}
	else
		d->bs->write(a);

	bytesWritten(size);
	return size;
}

void QSASL::bs_readyRead()
{
	if(!d->bs)
		return;

	QByteArray a = d->bs->read(8192);
	if(d->security > 0) {
		const char *out;
		unsigned int len;
		int r = sasl_decode(d->con, a.data(), a.size(), &out, &len);
		if(r != SASL_OK) {
			reset();
			error(ErrRead);
			return;
		}
		QByteArray b(len);
		memcpy(b.data(), out, len);
		if(!b.isEmpty()) {
			appendRead(b);
			readyRead();
		}
	}
	else {
		appendRead(a);
		readyRead();
	}
}
*/
