/* vi: set sw=4 ts=4:
 *
 * Copyright (C) 2001 - 2007 Christian Hohnstaedt.
 *
 * All rights reserved.
 */


#include "pki_evp.h"
#include "pass_info.h"
#include "func.h"
#include "db.h"
#include <openssl/rand.h>
#include <openssl/engine.h>
#include <qprogressdialog.h>
#include <qapplication.h>
#include <qdir.h>
#include <widgets/MainWindow.h>

char pki_evp::passwd[MAX_PASS_LENGTH]={0,};
char pki_evp::oldpasswd[MAX_PASS_LENGTH]={0,};

QString pki_evp::passHash = QString();

QPixmap *pki_evp::icon[2]= { NULL, NULL };

void pki_evp::erasePasswd()
{
	for (int i=0; i<MAX_PASS_LENGTH; i++)
		passwd[i] = 0;
}

void pki_evp::eraseOldPasswd()
{
	for (int i=0; i<MAX_PASS_LENGTH; i++)
		oldpasswd[i] = 0;
}

void pki_evp::setPasswd(const char *pass)
{
	strncpy(passwd, pass, MAX_PASS_LENGTH);
	passwd[MAX_PASS_LENGTH-1] = '\0';
}

void pki_evp::setOldPasswd(const char *pass)
{
	strncpy(oldpasswd, pass, MAX_PASS_LENGTH);
	oldpasswd[MAX_PASS_LENGTH-1] = '\0';
}

void pki_evp::init(int type)
{
	key = EVP_PKEY_new();
	key->type = type;
	class_name = "pki_evp";
	encKey = NULL;
	encKey_len = 0;
	ownPass = ptCommon;
	dataVersion=1;
	pkiType=asym_key;
	cols=5;
}

static void incProgress(int, int, void *progress)
{
	int i = ((QProgressBar *)progress)->value();
			((QProgressBar *)progress)->setValue(++i);
}

QString pki_evp::getTypeString()
{
	QString type;
	switch (EVP_PKEY_type(key->type)) {
		case EVP_PKEY_RSA:
			type = "RSA";
			break;
		case EVP_PKEY_DSA:
			type = "DSA";
			break;
		default:
			type = "---";
	}
	return type;
}

QString pki_evp::getIntNameWithType()
{
	return getIntName() + " (" + getTypeString() + ")";
}

QString pki_evp::removeTypeFromIntName(QString n)
{
	if (n.right(1) != ")" ) return n;
	n.truncate(n.length() - 6);
	return n;
}

void pki_evp::setOwnPass(enum passType x)
{
	EVP_PKEY *pk=NULL, *pk_back = key;
	int oldOwnPass = ownPass;

	if (ownPass == x || isPubKey())
		return;

	try {
		pk = decryptKey();
		if (pk == NULL)
			return;

		key = pk;
		ownPass = x;
		encryptKey();
	}
	catch (errorEx &err) {
		if (pk)
			EVP_PKEY_free(pk);
		key = pk_back;
		ownPass = oldOwnPass;
		throw(err);
	}
	EVP_PKEY_free(pk_back);
}

void pki_evp::generate(int bits, int type, QProgressBar *progress)
{
	RSA *rsakey = NULL;
	DSA *dsakey = NULL;

	progress->setMinimum(0);
	progress->setMaximum(100);
	progress->setValue(50);

	if (type == EVP_PKEY_RSA) {
		rsakey = RSA_generate_key(bits, 0x10001, &incProgress, progress);
		if (rsakey)
			EVP_PKEY_set1_RSA(key, rsakey);
	} else if (type == EVP_PKEY_DSA) {
		progress->setMaximum(500);
		dsakey = DSA_generate_parameters(bits, NULL, 0, NULL, NULL,
				&incProgress, progress);
		DSA_generate_key(dsakey);
		if(dsakey)
			EVP_PKEY_set1_DSA(key,dsakey);
	}
	openssl_error();
	encryptKey();
}

pki_evp::pki_evp(const pki_evp *pk)
	:pki_key(pk)
{
	int keylen;
	unsigned char *der_key, *p;

	init(pk->key->type);
	openssl_error();
	ownPass = pk->ownPass;
	encKey_len = pk->encKey_len;
	if (encKey_len) {
		encKey = (unsigned char *)OPENSSL_malloc(encKey_len);
		check_oom(encKey);
		memcpy(encKey, pk->encKey, encKey_len);
	}
	keylen = i2d_PublicKey(pk->key, NULL);
	der_key = (unsigned char *)OPENSSL_malloc(keylen);
	check_oom(der_key);
	p = der_key;
	i2d_PublicKey(pk->key, &p);
	p = der_key;
	D2I_CLASHT(d2i_PublicKey, pk->key->type, &key, &p, keylen);
	openssl_error();
	OPENSSL_free(der_key);
}

pki_evp::pki_evp(const QString name, int type )
	:pki_key(name)
{
	init(type);
	openssl_error();
}

pki_evp::pki_evp(EVP_PKEY *pkey)
	:pki_key()
{
	init();
	if (key) {
		EVP_PKEY_free(key);
	}
	key = pkey;
}

static bool EVP_PKEY_isPrivKey(EVP_PKEY *key)
{
	switch (EVP_PKEY_type(key->type)) {
		case EVP_PKEY_RSA:
			return key->pkey.rsa->d ? true: false;
		case EVP_PKEY_DSA:
			return key->pkey.dsa->priv_key ? true: false;
	}
	return false;
}

void pki_evp::fromPEM_BIO(BIO *bio, QString name)
{
	EVP_PKEY *pkey;
	int pos;
	pass_info p(XCA_TITLE, qApp->translate("MainWindow",
			"Please enter the password to decrypt the private key."));
	pos = BIO_tell(bio);
	pkey = PEM_read_bio_PrivateKey(bio, NULL, MainWindow::passRead, &p);
	if (!pkey){
		ign_openssl_error();
		pos = BIO_seek(bio, pos);
		pkey = PEM_read_bio_PUBKEY(bio, NULL, MainWindow::passRead, &p);
	}
	if (pkey){
		if (key)
			EVP_PKEY_free(key);
		key = pkey;
		if (EVP_PKEY_isPrivKey(key))
			bogusEncryptKey();
		setIntName(rmslashdot(name));
	}
	openssl_error(name);
}

void pki_evp::fload(const QString fname)
{
	pass_info p(XCA_TITLE, qApp->translate("MainWindow", "Please enter the password to decrypt the private key.")
		+ "\n'" + fname + "'");
	pem_password_cb *cb = MainWindow::passRead;
	FILE *fp = fopen(CCHAR(fname), "r");
	EVP_PKEY *pkey;

	ign_openssl_error();
	if (fp != NULL) {
		pkey = PEM_read_PrivateKey(fp, NULL, cb, &p);
		if (!pkey) {
			if (ERR_get_error() == 0x06065064) {
				fclose(fp);
				ign_openssl_error();
				throw errorEx(tr("Failed to decrypt the key (bad password) ") +
						fname, class_name);
			}
		}
		if (!pkey) {
			ign_openssl_error();
			rewind(fp);
			pkey = d2i_PrivateKey_fp(fp, NULL);
		}
		if (!pkey) {
			ign_openssl_error();
			rewind(fp);
			pkey = PEM_read_PUBKEY(fp, NULL, cb, &p);
		}
		if (!pkey) {
			ign_openssl_error();
			rewind(fp);
			pkey = d2i_PUBKEY_fp(fp, NULL);
		}
		if (pkey){
			if (key)
				EVP_PKEY_free(key);
			key = pkey;
			if (EVP_PKEY_isPrivKey(key))
				bogusEncryptKey();
			setIntName(rmslashdot(fname));
		}
		fclose(fp);
		openssl_error(fname);
	} else
		fopen_error(fname);
}

void pki_evp::fromData(const unsigned char *p, db_header_t *head )
{
	const unsigned char *p1;
	int version, type, size;

	p1 = p;
	size = head->len - sizeof(db_header_t);
	version = head->version;

	if (key)
		EVP_PKEY_free(key);

	key = NULL;
	type = db::intFromData(&p1);
	ownPass = db::intFromData(&p1);
	D2I_CLASHT(d2i_PublicKey, type, &key, &p1, size - (2*sizeof(int)));
	openssl_error();

	encKey_len = size - (p1-p);
	if (encKey_len) {
		encKey = (unsigned char *)OPENSSL_malloc(encKey_len);
		check_oom(encKey);
		memcpy(encKey, p1 ,encKey_len);
	}

}

EVP_PKEY *pki_evp::decryptKey() const
{
	unsigned char *p;
	const unsigned char *p1;
	int outl, decsize;
	unsigned char iv[EVP_MAX_IV_LENGTH];
	unsigned char ckey[EVP_MAX_KEY_LENGTH];

	EVP_PKEY *tmpkey;
	EVP_CIPHER_CTX ctx;
	const EVP_CIPHER *cipher = EVP_des_ede3_cbc();
	char ownPassBuf[MAX_PASS_LENGTH] = "";

	if (isPubKey()) {
		unsigned char *q;
		outl = i2d_PublicKey(key, NULL);
		q = (unsigned char *)OPENSSL_malloc(outl);
		check_oom(q);
		p = q;
		i2d_PublicKey(key, &p);
		p = q;
		tmpkey = D2I_CLASHT(d2i_PublicKey, key->type, NULL, &p, outl);
		OPENSSL_free(q);
		return tmpkey;
	}
	/* This key has its own password */
	if (ownPass == ptPrivate) {
		int ret;
		pass_info pi(XCA_TITLE, qApp->translate("MainWindow",
			"Please enter the password to decrypt the private key: '") +
			getIntName() + "'");
		ret = MainWindow::passRead(ownPassBuf, MAX_PASS_LENGTH, 0, &pi);
		if (ret < 0)
			throw errorEx(tr("Password input aborted"), class_name);
	} else if (ownPass == ptBogus) { // BOGUS pass
		ownPassBuf[0] = '\0';
	} else {
		memcpy(ownPassBuf, passwd, MAX_PASS_LENGTH);
		//printf("Orig password: '%s' len:%d\n", passwd, strlen(passwd));
		while (md5passwd(ownPassBuf) != passHash &&
			sha512passwd(ownPassBuf, passHash) != passHash)
		{
			int ret;
			//printf("Passhash= '%s', new hash= '%s', passwd= '%s'\n",
				//CCHAR(passHash), CCHAR(md5passwd(ownPassBuf)), ownPassBuf);
			pass_info p(XCA_TITLE, qApp->translate("MainWindow",
					"Please enter the database password for decrypting the key"));
			ret = MainWindow::passRead(ownPassBuf, MAX_PASS_LENGTH, 0, &p);
			if (ret < 0)
				throw errorEx(tr("Password input aborted"), class_name);
		}
	}
	//printf("Using decrypt Pass: %s\n", ownPassBuf);
	p = (unsigned char *)OPENSSL_malloc(encKey_len);
	check_oom(p);
	openssl_error();
	p1 = p;
	memset(iv, 0, EVP_MAX_IV_LENGTH);

	memcpy(iv, encKey, 8); /* recover the iv */
	/* generate the key */
	EVP_BytesToKey(cipher, EVP_sha1(), iv, (unsigned char *)ownPassBuf,
		strlen(ownPassBuf), 1, ckey,NULL);
	/* we use sha1 as message digest,
	 * because an md5 version of the password is
	 * stored in the database...
	 */
	EVP_CIPHER_CTX_init (&ctx);
	EVP_DecryptInit( &ctx, cipher, ckey, iv);
	EVP_DecryptUpdate( &ctx, p , &outl, encKey + 8, encKey_len - 8 );
	decsize = outl;
	EVP_DecryptFinal( &ctx, p + decsize , &outl );
	decsize += outl;
	//printf("Decrypt decsize=%d, encKey_len=%d\n", decsize, encKey_len);
	openssl_error();
	tmpkey = D2I_CLASHT(d2i_PrivateKey, key->type, NULL, &p1, decsize);
	OPENSSL_free(p);
	EVP_CIPHER_CTX_cleanup(&ctx);
	openssl_error();
	return tmpkey;
}

unsigned char *pki_evp::toData(int *size)
{
	unsigned char *p, *p1;
	int pubsize;

	pubsize = i2d_PublicKey(key, NULL);
	*size = pubsize + encKey_len + (2*sizeof(int));
	p = (unsigned char *)OPENSSL_malloc(*size);
	check_oom(p);
	openssl_error();
	p1 = p;
	db::intToData(&p1, key->type);
	db::intToData(&p1, ownPass);
	i2d_PublicKey(key, &p1);
	openssl_error();
	if (encKey_len) {
		memcpy(p1, encKey, encKey_len);
	}
	// printf("To data: pubsize=%d, encKey_len: %d, *size=%d\n",
			//pubsize, encKey_len, *size);
	return p;
}

void pki_evp::encryptKey(const char *password)
{
	int outl, keylen;
	EVP_PKEY *pkey1 = NULL;
	EVP_CIPHER_CTX ctx;
	const EVP_CIPHER *cipher = EVP_des_ede3_cbc();
	unsigned char iv[EVP_MAX_IV_LENGTH], *punenc, *punenc1;
	const unsigned char *punencc;
	unsigned char ckey[EVP_MAX_KEY_LENGTH];
	char ownPassBuf[MAX_PASS_LENGTH];

	/* This key has its own, private password */
	if (ownPass == ptPrivate) {
		int ret;
		pass_info p(XCA_TITLE, qApp->translate("MainWindow",
			"Please enter the password to protect the private key: '") +
			getIntName() + "'");
		ret = MainWindow::passWrite(ownPassBuf, MAX_PASS_LENGTH, 0, &p);
		if (ret < 0)
			throw errorEx("Password input aborted", class_name);
	} else if (ownPass == ptBogus) { // BOGUS password
		ownPassBuf[0] = '\0';
	} else {
		if (password) {
			/* use the password parameter if this is a common password */
			strncpy(ownPassBuf, password, MAX_PASS_LENGTH);
		} else {
			int ret = 0;
			memcpy(ownPassBuf, passwd, MAX_PASS_LENGTH);
			pass_info p(XCA_TITLE, qApp->translate("MainWindow",
				"Please enter the database password for encrypting the key"));
			while (md5passwd(ownPassBuf) != passHash &&
				sha512passwd(ownPassBuf, passHash) != passHash )
			{
				ret = MainWindow::passRead(ownPassBuf, MAX_PASS_LENGTH, 0,&p);
				if (ret < 0)
					throw errorEx("Password input aborted", class_name);
			}
		}
	}

	/* Prepare Encryption */
	memset(iv, 0, EVP_MAX_IV_LENGTH);
	RAND_pseudo_bytes(iv,8);      /* Generate a salt */
	EVP_BytesToKey(cipher, EVP_sha1(), iv, (unsigned char *)ownPassBuf,
			strlen(ownPassBuf), 1, ckey, NULL);
	EVP_CIPHER_CTX_init (&ctx);
	openssl_error();
	if (encKey)
		OPENSSL_free(encKey);
	encKey_len = 0;

	/* reserve space for unencrypted and encrypted key */
	keylen = i2d_PrivateKey(key, NULL);
	encKey = (unsigned char *)OPENSSL_malloc(keylen + EVP_MAX_KEY_LENGTH + 8);
	check_oom(encKey);
	punenc = (unsigned char *)OPENSSL_malloc(keylen);
	check_oom(punenc);
	openssl_error();
	punencc = punenc1 = punenc;
	memcpy(encKey, iv, 8); /* store the iv */
	/* convert rsa/dsa to Pubkey */
	i2d_PublicKey(key, &punenc);
	punenc = punenc1;
	D2I_CLASHT(d2i_PublicKey, key->type, &pkey1, &punencc, keylen);
	openssl_error();
	i2d_PrivateKey(key, &punenc);
	punenc = punenc1;
	/*
	 * Now DER version of privkey is in punenc, pubkey is in pkey1
	 * and privkey is still in key
	 */

	/* do the encryption */
	/* store key right after the iv */
	EVP_EncryptInit( &ctx, cipher, ckey, iv);
	EVP_EncryptUpdate( &ctx, encKey + 8, &outl, punenc, keylen );
	encKey_len = outl;
	EVP_EncryptFinal( &ctx, encKey + encKey_len + 8, &outl );
	encKey_len += outl + 8;

	/* Cleanup */
	EVP_CIPHER_CTX_cleanup(&ctx);
	/* wipe out the memory */
	memset(punenc, 0, keylen);
	OPENSSL_free(punenc);
	openssl_error();

	EVP_PKEY_free(key);
	key = pkey1;
	openssl_error();

	//CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_OFF);

	//printf("Encrypt: encKey_len=%d\n", encKey_len);
	return;
}

void pki_evp::bogusEncryptKey()
{
	ownPass = ptBogus;
	encryptKey();
}

pki_evp::~pki_evp()
{
	if (key)
		EVP_PKEY_free(key);
	if (encKey)
		OPENSSL_free(encKey);
}


void pki_evp::writePKCS8(const QString fname, const EVP_CIPHER *enc,
		pem_password_cb *cb, bool pem)
{
	EVP_PKEY *pkey;
	pass_info p(XCA_TITLE, qApp->translate("MainWindow",
				"Please enter the password protecting the PKCS#8 key"));
	FILE *fp = fopen(fname.toAscii(),"w");
	if (fp != NULL) {
		if (key) {
			pkey = decryptKey();
			if (pkey) {
				if (pem)
					PEM_write_PKCS8PrivateKey(fp, pkey, enc, NULL, 0, cb, &p);
				else
					i2d_PKCS8PrivateKey_fp(fp, pkey, enc, NULL, 0, cb, &p);
				EVP_PKEY_free(pkey);
			}
		}
		fclose(fp);
		openssl_error();
	} else
		fopen_error(fname);
}

static int mycb(char *buf, int size, int, void *)
{
	strncpy(buf, pki_evp::passwd, size);
	return strlen(pki_evp::passwd);
}

void pki_evp::writeDefault(const QString fname)
{
	writeKey(fname + QDir::separator() + getIntName() + ".pem",
			EVP_des_ede3_cbc(), mycb, true);
}

void pki_evp::writeKey(const QString fname, const EVP_CIPHER *enc,
			pem_password_cb *cb, bool pem)
{
	EVP_PKEY *pkey;
	pass_info p(XCA_TITLE, qApp->translate("MainWindow", "Please enter the export password for the private key"));
	if (isPubKey()) {
		writePublic(fname, pem);
		return;
	}
	FILE *fp = fopen(CCHAR(fname), "w");
	if (!fp) {
		fopen_error(fname);
		return;
	}
	if (key){
		pkey = decryptKey();
		if (pkey) {
			if (pem) {
				PEM_write_PrivateKey(fp, pkey, enc, NULL, 0, cb, &p);
			} else {
				i2d_PrivateKey_fp(fp, pkey);
			}
			EVP_PKEY_free(pkey);
		}
		openssl_error();
	}
	fclose(fp);
}

void pki_evp::writePublic(const QString fname, bool pem)
{
	FILE *fp = fopen(fname.toAscii(),"w");
	if (fp == NULL) {
		fopen_error(fname);
		return;
	}
	if (pem)
		PEM_write_PUBKEY(fp, key);
	else
		i2d_PUBKEY_fp(fp, key);

	fclose(fp);
	openssl_error();
}


QString pki_evp::length()
{
	char st[64];
	QString x;
	if (key->type == EVP_PKEY_DSA && key->pkey.dsa->p == NULL) {
		return x="???";
	}
	sprintf(st,"%i bit",  EVP_PKEY_bits(key) );
	openssl_error();
	x = st;
	return x;
}

QString pki_evp::BN2QString(BIGNUM *bn)
{
	if (bn == NULL) return "--";
	QString x="";
	char zs[10];
	int j;
	int size = BN_num_bytes(bn);
	unsigned char *buf = (unsigned char *)OPENSSL_malloc(size);
	check_oom(buf);
	BN_bn2bin(bn, buf);
	for (j = 0; j< size; j++) {
		sprintf(zs, "%02X%c",buf[j], ((j+1)%16 == 0) ?'\n':':');
		x += zs;
	}
	OPENSSL_free(buf);
	openssl_error();
	return x;
}

QString pki_evp::modulus() {
	if (key->type != EVP_PKEY_RSA) return "??no rsa??";
	return BN2QString(key->pkey.rsa->n);
}

QString pki_evp::pubEx() {
	if (key->type != EVP_PKEY_RSA) return "??no rsa??";
	return BN2QString(key->pkey.rsa->e);
}

QString pki_evp::subprime() {
	if (key->type != EVP_PKEY_DSA) return "??no dsa??";
	return BN2QString(key->pkey.dsa->q);
}

QString pki_evp::pubkey() {
	if (key->type != EVP_PKEY_DSA) return "??no dsa??";
	return BN2QString(key->pkey.dsa->pub_key);
}

bool pki_evp::compare(pki_base *ref)
{
	if (ref->getType() != getType())
		return false;
	pki_evp *kref = (pki_evp *)ref;
	if(key->type == EVP_PKEY_RSA) {
		if (kref==NULL || kref->key==NULL || kref->key->pkey.rsa->n==NULL)
			return false;
		if (key == NULL || key->pkey.rsa->n == NULL)
			return false;
		if (
			BN_cmp(key->pkey.rsa->n, kref->key->pkey.rsa->n) ||
			BN_cmp(key->pkey.rsa->e, kref->key->pkey.rsa->e)
		){
			openssl_error();
			return false;
		}
	} else if(key->type == EVP_PKEY_DSA) {
		if(kref==NULL || kref->key==NULL || kref->key->pkey.dsa->pub_key==NULL)
			return false;
		if(key==NULL || key->pkey.dsa->pub_key==NULL)
			return false;
		if(BN_cmp(key->pkey.dsa->pub_key,kref->key->pkey.dsa->pub_key)){
			openssl_error();
			return false;
		}
	}
	openssl_error();
	return true;
}


bool pki_evp::isPubKey() const
{
	if (encKey_len == 0 || encKey == NULL) {
		return true;
	}
	return false;
}

int pki_evp::verify()
{
	bool veri = false;
	return true;
	if (key->type == EVP_PKEY_RSA && isPrivKey()) {
		if (RSA_check_key(key->pkey.rsa) == 1)
			veri = true;
	}
	if (isPrivKey())
		veri = true;
	openssl_error();
	return veri;
}

int pki_evp::getType()
{
	return key->type;
}

const EVP_MD *pki_evp::getDefaultMD()
{
	const EVP_MD *md;
	switch (key->type) {
		case EVP_PKEY_RSA: md = EVP_sha1(); break;
		case EVP_PKEY_DSA: md = EVP_dss1(); break;
		default: md = NULL; break;
	}
	return md;
}

QVariant pki_evp::column_data(int col)
{
	QStringList sl;
	sl << tr("Common") << tr("Private") << tr("Bogus");
	switch (col) {
		case 0:
			return QVariant(getIntName());
		case 1:
			return QVariant(getTypeString());
		case 2:
			return QVariant(length());
		case 3:
			return QVariant(getUcount());
		case 4:
			if (isPubKey())
				return QVariant(tr("No password"));
			if (ownPass<0 || ownPass>2)
				return QVariant("Holla die Waldfee");
			return QVariant(sl[ownPass]);
	}
	return QVariant();
}

QVariant pki_evp::getIcon()
{
	int pixnum= isPubKey() ? 1 : 0;
	return QVariant(*icon[pixnum]);
}

QString pki_evp::md5passwd(const char *pass)
{

	EVP_MD_CTX mdctx;
	QString str;
	int n;
	int j;
	unsigned char m[EVP_MAX_MD_SIZE];
	EVP_DigestInit(&mdctx, EVP_md5());
	EVP_DigestUpdate(&mdctx, pass, strlen(pass));
	EVP_DigestFinal(&mdctx, m, (unsigned*)&n);
	for (j=0; j<n; j++) {
		char zs[4];
		sprintf(zs, "%02X%c",m[j], (j+1 == n) ?'\0':':');
		str += zs;
	}
	return str;
}

QString pki_evp::sha512passwd(QString pass, QString salt)
{

	EVP_MD_CTX mdctx;
	QString str;
	int n;
	int j;
	unsigned char m[EVP_MAX_MD_SIZE];

	if (salt.length() <5)
		abort();

	str = salt.left(5);
	pass = str + pass;

	EVP_DigestInit(&mdctx, EVP_sha512());
	EVP_DigestUpdate(&mdctx, CCHAR(pass), pass.size());
	EVP_DigestFinal(&mdctx, m, (unsigned*)&n);

	for (j=0; j<n; j++) {
		char zs[4];
		sprintf(zs, "%02X",m[j]);
		str += zs;
	}
	return str;
}

void pki_evp::veryOldFromData(unsigned char *p, int size )
{
	unsigned char *sik, *pdec, *pdec1, *sik1;
	int outl, decsize;
	unsigned char iv[EVP_MAX_IV_LENGTH];
	unsigned char ckey[EVP_MAX_KEY_LENGTH];
	memset(iv, 0, EVP_MAX_IV_LENGTH);
	RSA *rsakey;
	EVP_CIPHER_CTX ctx;
	const EVP_CIPHER *cipher = EVP_des_ede3_cbc();
	sik = (unsigned char *)OPENSSL_malloc(size);
	check_oom(sik);
	openssl_error();
	pdec = (unsigned char *)OPENSSL_malloc(size);
	if (pdec == NULL ) {
		OPENSSL_free(sik);
		check_oom(pdec);
	}
	pdec1=pdec;
	sik1=sik;
	memcpy(iv, p, 8); /* recover the iv */
	/* generate the key */
	EVP_BytesToKey(cipher, EVP_sha1(), iv, (unsigned char *)oldpasswd,
		strlen(oldpasswd), 1, ckey,NULL);
	/* we use sha1 as message digest,
	 * because an md5 version of the password is
	 * stored in the database...
	 */
	EVP_CIPHER_CTX_init (&ctx);
	EVP_DecryptInit( &ctx, cipher, ckey, iv);
	EVP_DecryptUpdate( &ctx, pdec , &outl, p + 8, size -8 );
	decsize = outl;
	EVP_DecryptFinal( &ctx, pdec + decsize , &outl );
	decsize += outl;
	openssl_error();
	memcpy(sik, pdec, decsize);
	if (key->type == EVP_PKEY_RSA) {
		rsakey=d2i_RSAPrivateKey(NULL,(const unsigned char **)&pdec, decsize);
		if (ign_openssl_error()) {
			rsakey=D2I_CLASH(d2i_RSA_PUBKEY, NULL, &sik, decsize);
		}
		openssl_error();
		if (rsakey) EVP_PKEY_assign_RSA(key, rsakey);
	}
	OPENSSL_free(sik1);
	OPENSSL_free(pdec1);
	EVP_CIPHER_CTX_cleanup(&ctx);
	openssl_error();
	encryptKey();
}

void pki_evp::oldFromData(unsigned char *p, int size )
{
	const unsigned char *p1;
	int version, type;

	p1 = (const unsigned char*)p;
	version = intFromData(&p1);
	if (version != 1) { // backward compatibility
		veryOldFromData(p, size);
		return;
	}
	if (key)
		EVP_PKEY_free(key);

	key = NULL;
	type = intFromData(&p1);
	ownPass = intFromData(&p1);

	D2I_CLASHT(d2i_PublicKey, type, &key, &p1, size - (2*sizeof(int)));
	openssl_error();

	encKey_len = size - (p1-p);
	if (encKey_len) {
		encKey = (unsigned char *)OPENSSL_malloc(encKey_len);
		check_oom(encKey);
		memcpy(encKey, p1 ,encKey_len);
	}

}
