#include "AppxSignature.hpp"
#include "Exceptions.hpp"
#include "FileStream.hpp"
#include "SignatureValidator.hpp"
#include "AppxCerts.hpp"

#include <string>
#include <sstream>
#include <iostream>

#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs7.h>
#include <openssl/pem.h>
#include <openssl/crypto.h>

namespace xPlat
{
    struct unique_BIO_deleter {
        void operator()(BIO *b) const { if (b) BIO_free(b); };
    };
    
    struct unique_PKCS7_deleter {
        void operator()(PKCS7 *p) const { if (p) PKCS7_free(p); };
    };

    struct unique_X509_deleter {
        void operator()(X509 *x) const { if (x) X509_free(x); };
    };
    
    struct unique_X509_STORE_deleter {
        void operator()(X509_STORE *xs) const { if (xs) X509_STORE_free(xs); };
    };

    struct unique_X509_STORE_CTX_deleter {
        void operator()(X509_STORE_CTX *xsc) const { if (xsc) {X509_STORE_CTX_cleanup(xsc); X509_STORE_CTX_free(xsc);} };
    };

    struct unique_X509_NAME_deleter {
        void operator()(X509_NAME *xn) const { if (xn) OPENSSL_free(xn); };
    };

    struct unique_STACK_X509_deleter {
        void operator()(STACK_OF(X509) *sx) const { if (sx) sk_X509_free(sx); };
    };

    typedef std::unique_ptr<BIO, unique_BIO_deleter> unique_BIO;
    typedef std::unique_ptr<PKCS7, unique_PKCS7_deleter> unique_PKCS7;
    typedef std::unique_ptr<X509, unique_X509_deleter> unique_X509;
    typedef std::unique_ptr<X509_STORE, unique_X509_STORE_deleter> unique_X509_STORE;
    typedef std::unique_ptr<X509_STORE_CTX, unique_X509_STORE_CTX_deleter> unique_X509_STORE_CTX;
    typedef std::unique_ptr<X509_NAME, unique_X509_NAME_deleter> unique_X509_NAME;
    typedef std::unique_ptr<STACK_OF(X509), unique_STACK_X509_deleter> unique_STACK_X509;
    
    // Best effort to determine whether the signature file is associated with a store cert
    static bool IsStoreOrigin(std::uint8_t* signatureBuffer, std::uint32_t cbSignatureBuffer)
    {
        unique_BIO bmem(BIO_new_mem_buf(signatureBuffer, cbSignatureBuffer));
        unique_PKCS7 p7(d2i_PKCS7_bio(bmem.get(), nullptr));

        STACK_OF(X509) *certStack = p7.get()->d.sign->cert;
        for (int i = 0; i < sk_X509_num(certStack); i++)
        {
            X509* cert = sk_X509_value(certStack, i);
            STACK_OF(X509_EXTENSION) *exts = cert->cert_info->extensions;
            for (int i = 0; i < sk_X509_EXTENSION_num(exts); i++) 
            {
                X509_EXTENSION *ext = sk_X509_EXTENSION_value(exts, i);
                if (ext)
                {
                    if (X509_EXTENSION_get_object(ext))
                    {
                        unique_BIO extbio(BIO_new(BIO_s_mem()));
                        if (!X509V3_EXT_print(extbio.get(), ext, 0, 0)) {
                            M_ASN1_OCTET_STRING_print(extbio.get(), ext->value);
                        }

                        BUF_MEM *bptr = nullptr;
                        BIO_get_mem_ptr(extbio.get(), &bptr);
                        
                        if (bptr && bptr->data && 
                            std::string((char*)bptr->data).find(std::string(OID::WindowsStore)) != std::string::npos)
                        {
                            return true;
                        }
                    }
                }
                
            }
        }
        return false;
    }

    // Best effort to determine whether the signature file is associated with a store cert
    static bool IsAuthenticodeOrigin(std::uint8_t* signatureBuffer, std::uint32_t cbSignatureBuffer)
    {
        bool retValue = false;
        return retValue;
    }

    static bool ConvertBase64Certificate(std::string base64CertWithDelimiters, std::vector<std::uint8_t>& decodedCert)
    {   
        std::istringstream stringStream(base64CertWithDelimiters);
        std::string base64Cert;     
        std::string line;
        while (std::getline(stringStream, line)) 
        {
            if (line.find("-----BEGIN CERTIFICATE-----") == std::string::npos &&
                line.find("-----END CERTIFICATE-----") == std::string::npos) {
                base64Cert += line; }
        }

        // Load a BIO filter with the base64Cert
        unique_BIO bsrc(BIO_new_mem_buf(base64Cert.data(), base64Cert.size()));

        // Put a Base64 decoder on the front of it
        unique_BIO b64(BIO_push(BIO_new(BIO_f_base64()), bsrc.get()));
        // Ignore new lines
	    BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);

        // Calculate how big the decode buffer needs to be
        int length = base64Cert.size();
        int padding = 0;
        char *base64CertT = (char*)base64Cert.data();
        if (base64CertT[length-1] == '=')
            padding++;
        if (base64CertT[length-2] == '=')
            padding++;
        
        // Resize the decoder buffer to the calculated length
        decodedCert.resize(((length * 3)/4) - padding);
        
        // Read the Base64 certificate thru the Base64 decoder into decodedCert 
        ThrowErrorIf(Error::AppxSignatureInvalid, 
            (BIO_read(b64.get(), (void*)decodedCert.data(), decodedCert.size()) != length),
            "Certificate is invalid");

        return true;
    }

    int VerifyCallback(int ok, X509_STORE_CTX *ctx)
    {
        if (!ok)
        {
            X509 *currentCert = X509_STORE_CTX_get_current_cert(ctx);
            int certError = X509_STORE_CTX_get_error(ctx);
            int depth = X509_STORE_CTX_get_error_depth(ctx);
            //printCert(currentCert);
            printf("Error depth %d, certError %d", depth, certError);
        }
        return ok; 
    }


    void PrintCertExtensions(X509* cert)
    {
        #define EXTNAME_LEN 256
        #define IFNEG_FAIL(a, b) {if (a < 0) {printf(b); return;}}
        #define IFNULL_FAIL(a, b) {if (!a) {printf(b); return;}}

        STACK_OF(X509_EXTENSION) *exts = cert->cert_info->extensions;

        int num_of_exts;
        if (exts) {
            num_of_exts = sk_X509_EXTENSION_num(exts);
        } else {
            num_of_exts = 0;
        }

        IFNEG_FAIL(num_of_exts, "error parsing number of X509v3 extensions.");

        for (int i=0; i < num_of_exts; i++) {

            printf("-----------------------------------------\n");
            X509_EXTENSION *ex = sk_X509_EXTENSION_value(exts, i);
            IFNULL_FAIL(ex, "unable to extract extension from stack");
            ASN1_OBJECT *obj = X509_EXTENSION_get_object(ex);
            IFNULL_FAIL(obj, "unable to extract ASN1 object from extension");

            BIO *ext_bio = BIO_new(BIO_s_mem());
            IFNULL_FAIL(ext_bio, "unable to allocate memory for extension value BIO");
            if (!X509V3_EXT_print(ext_bio, ex, 0, 0)) {
                M_ASN1_OCTET_STRING_print(ext_bio, ex->value);
            }

            BUF_MEM *bptr;
            BIO_get_mem_ptr(ext_bio, &bptr);
            BIO_set_close(ext_bio, BIO_NOCLOSE);

            // remove newlines
            int lastchar = bptr->length;
            if (lastchar > 1 && (bptr->data[lastchar-1] == '\n' || bptr->data[lastchar-1] == '\r')) {
                bptr->data[lastchar-1] = (char) 0;
            }
            if (lastchar > 0 && (bptr->data[lastchar] == '\n' || bptr->data[lastchar] == '\r')) {
                bptr->data[lastchar] = (char) 0;
            }

            BIO_free(ext_bio);

            unsigned nid = OBJ_obj2nid(obj);
            if (nid == NID_undef) {
                // no lookup found for the provided OID so nid came back as undefined.
                char extname[EXTNAME_LEN];
                OBJ_obj2txt(extname, EXTNAME_LEN, (const ASN1_OBJECT *) obj, 1);
                printf("extension name is %s\n", extname);
            } else {
                // the OID translated to a NID which implies that the OID has a known sn/ln
                const char *c_ext_name = OBJ_nid2ln(nid);
                IFNULL_FAIL(c_ext_name, "invalid X509v3 extension name");
                printf("extension name is %s\n", c_ext_name);
            }

            printf("extension length is %u\n", (unsigned)bptr->length);
            printf("extension value is %s\n", bptr->data);
        }
    }

    bool SignatureValidator::Validate(
        /*in*/ APPX_VALIDATION_OPTION option, 
        /*in*/ IStream *stream, 
        /*inout*/ std::map<xPlat::AppxSignatureObject::DigestName, xPlat::AppxSignatureObject::Digest>& digests,
        /*inout*/ SignatureOrigin&)
    {
        // If the caller wants to skip signature validation altogether, just bug out early. We will not read the digests
        if (option & APPX_VALIDATION_OPTION_SKIPSIGNATURE) { return false; }

        LARGE_INTEGER start = {0};
        ULARGE_INTEGER end = {0};
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::END, &end));
        ThrowErrorIf(Error::AppxSignatureInvalid, (end.QuadPart <= sizeof(P7X_FILE_ID) || end.QuadPart > (2 << 20)), "stream is too big");
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::START, nullptr));

        std::uint32_t fileID = 0;
        ThrowHrIfFailed(stream->Read(&fileID, sizeof(fileID), nullptr));
        ThrowErrorIf(Error::AppxSignatureInvalid, (fileID != P7X_FILE_ID), "unexpected p7x header");

        std::uint32_t p7sSize = end.u.LowPart - sizeof(fileID);
        std::vector<std::uint8_t> p7s(p7sSize);
        ULONG actualRead = 0;
        ThrowHrIfFailed(stream->Read(p7s.data(), p7s.size(), &actualRead));
        ThrowErrorIf(Error::AppxSignatureInvalid, (actualRead != p7s.size()), "read error");

        // Tell OpenSSL to use all available algorithms when evaluating certs
        OpenSSL_add_all_algorithms();

        // Create a trusted cert store
        unique_X509_STORE store(X509_STORE_new());
        unique_STACK_X509 trustedChain(sk_X509_new_null());
        
        // Loop through our trusted PEM certs, create X509 objects from them, and add to trusted store
        for ( std::string s : appxCerts )
        {
            // Load the cert into memory
            unique_BIO bcert(BIO_new_mem_buf(s.data(), s.size()));

            // Create a cert from the memory buffer
            unique_X509 cert(PEM_read_bio_X509(bcert.get(), nullptr, nullptr, nullptr));
            
            // Add the cert to the trusted store
            ThrowErrorIfNot(Error::AppxSignatureInvalid, 
                X509_STORE_add_cert(store.get(), cert.get()) == 1, 
                "Could not add cert to keychain");
            
            sk_X509_push(trustedChain.get(), cert.get());
        }

        // TODO: read digests
        unique_BIO bmem(BIO_new_mem_buf(p7s.data(), p7s.size()));
        unique_PKCS7 p7(d2i_PKCS7_bio(bmem.get(), nullptr));

        //unique_BIO content(BIO_new(BIO_s_mem()));
        X509_STORE_set_verify_cb(store.get(), &VerifyCallback);
        X509_STORE_set_purpose(store.get(), X509_PURPOSE_ANY);
        
        //ThrowErrorIfNot(Error::AppxSignatureInvalid, 
        //    PKCS7_verify(p7.get(), nullptr, store.get(), nullptr/*indata*/, nullptr/*out*/, PKCS7_NOCRL/*flags*/) == 1, 
        //    "Could not verify package signature");

        //You need to create a certificate store using X509_STORE_CTX_new. 
        //Then add certificate chain using X509_STORE_CTX_set_chain. 
        //Add trusted root certificate using X509_STORE_CTX_trusted_stack. 
        //Finally add certificate to be verified using X509_STORE_CTX_set_cert.
        
        
        STACK_OF(X509) *untrustedCerts = p7.get()->d.sign->cert;
        for (int i = 0; i < sk_X509_num(untrustedCerts); i++)
        {
            X509* cert = sk_X509_value(untrustedCerts, i);
            unique_X509_STORE_CTX context(X509_STORE_CTX_new());
            X509_STORE_CTX_init(context.get(), store.get(), nullptr, nullptr);

            X509_STORE_CTX_set_chain(context.get(), untrustedCerts);
            X509_STORE_CTX_trusted_stack(context.get(), trustedChain.get());
            X509_STORE_CTX_set_cert(context.get(), cert);

            X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(context.get());
            X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CB_ISSUER_CHECK|X509_V_FLAG_TRUSTED_FIRST);

            //PrintCertExtensions(cert);

            ThrowErrorIfNot(Error::AppxSignatureInvalid, 
                X509_verify_cert(context.get()) == 1, 
                "Could not verify cert");
        }

        ThrowErrorIfNot(Error::AppxSignatureInvalid, (
            IsStoreOrigin(p7s.data(), p7s.size()) ||
            IsAuthenticodeOrigin(p7s.data(), p7s.size()) ||
            (option & APPX_VALIDATION_OPTION::APPX_VALIDATION_OPTION_ALLOWSIGNATUREORIGINUNKNOWN)
        ), "Signature origin check failed");
        return true;
    }

} // namespace xPlat