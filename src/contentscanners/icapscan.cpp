// ICAP server content scanning plugin

// For all support, instructions and copyright go to:
// http://e2guardian.org/
// Released under the GPL v2, with the OpenSSL exception described in the README file.

// INCLUDES
#ifdef HAVE_CONFIG_H
#include "e2config.h"
#endif

#include "../String.hpp"

#include "../ContentScanner.hpp"
#include "../OptionContainer.hpp"
#include "../Logger.hpp"

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h> // for gethostby
#include <cstdio>

// DEFINES

#define ICAP_CONTINUE E2CS_MAX + 1
#define ICAP_NODATA E2CS_MAX + 2

// GLOBALS

extern OptionContainer o;

// DECLARATIONS

// class name is relevant!
class icapinstance : public CSPlugin
{
    public:
    icapinstance(ConfigVar &definition)
        : CSPlugin(definition), usepreviews(false), previewsize(0), supportsXIF(false), needsBody(false){};

    int willScanRequest(const String &url, const char *user, FOptionContainer* &foc, const char *ip, bool post,
        bool reconstituted, bool exception, bool bypass);

    int scanMemory(HTTPHeader *requestheader, HTTPHeader *docheader, const char *user, FOptionContainer* &foc,
        const char *ip, const char *object, unsigned int objectsize, NaughtyFilter *checkme,
        const String *disposition, const String *mimetype);
    int scanFile(HTTPHeader *requestheader, HTTPHeader *docheader, const char *user, FOptionContainer* &foc,
        const char *ip, const char *filename, NaughtyFilter *checkme,
        const String *disposition, const String *mimetype);

    int init(void *args);

    private:
    // ICAP server hostname, IP and port
    String icaphost;
    String icapip;
    unsigned int icapport;
    // URL for the AV service
    String icapurl;
    // whether or not to send ICAP message previews, and the preview object size
    bool usepreviews;
    unsigned int previewsize;
    // supports X-Infection-Found and/or needs us to look at the whole body
    bool supportsXIF;
    bool needsBody;

    // Send ICAP request headers to server
    bool doHeaders(Socket &icapsock, HTTPHeader *reqheader, HTTPHeader *respheader, unsigned int objectsize);
    // Check data returned from ICAP server and return one of our standard return codes
    int doScan(Socket &icapsock, HTTPHeader *docheader, const char *object, unsigned int objectsize, NaughtyFilter *checkme);
};

// IMPLEMENTATION

// class factory code *MUST* be included in every plugin

CSPlugin *icapcreate(ConfigVar &definition)
{
    return new icapinstance(definition);
}

// end of Class factory

// don't scan POST data or reconstituted data - wouldn't work for multi-part posts
// without faking request headers, as we are only passed a single part, not the whole request verbatim
int icapinstance::willScanRequest(const String &url, const char *user, FOptionContainer* &foc, const char *ip,
    bool post, bool reconstituted, bool exception, bool bypass)
{
    if (post || reconstituted)
        return E2CS_NOSCAN;
    else {
        return CSPlugin::willScanRequest(url, user, foc, ip,
            post, reconstituted, exception, bypass);
    }
}

// initialise the plugin - determine icap ip, port & url
int icapinstance::init(void *args)
{
    // always include these lists
    if (!readStandardLists()) {
        return E2CS_ERROR;
    }

    icapurl = cv["icapurl"]; // format: icap://icapserver:1344/avscan
    if (icapurl.length() < 3) {
        e2logger_error("Error reading icapurl option.");
        return E2CS_ERROR;
        // it would be far better to do a test connection
    }
    icaphost = icapurl.after("//");
    icapport = icaphost.after(":").before("/").toInteger();
    if (icapport == 0) {
        icapport = 1344;
    }
    icaphost = icaphost.before("/");
    if (icaphost.contains(":")) {
        icaphost = icaphost.before(":");
    }
    struct hostent *host;
    if ((host = gethostbyname(icaphost.toCharArray())) == 0) {
        e2logger_error("Error resolving icap host address.");
        return E2CS_ERROR;
    }
    icapip = inet_ntoa(*(struct in_addr *)host->h_addr_list[0]);

    e2logger_debugicapc("ICAP server is ", icapip );

    // try to connect to the ICAP server and perform an OPTIONS request
    Socket icapsock;
    try {
        if (icapsock.connect(icapip.toCharArray(), icapport) < 0) {
            throw std::runtime_error("Could not connect to server");
        }
        String line("OPTIONS " + icapurl + " ICAP/1.0\r\nHost: " + icaphost + "\r\n\r\n");
        icapsock.writeString(line.toCharArray());
        // parse the response
        char buff[8192];
        // first line - look for 200 OK
        icapsock.getLine(buff, 8192, o.content.content_scanner_timeout);
        line = buff;

        e2logger_debugicapc("ICAP/1.0 OPTIONS response: ", line);

        if (line.after(" ").before(" ") != "200") {
            e2logger_error("ICAP response not 200 OK");
            return E2CS_WARNING;
            //throw std::runtime_error("Response not 200 OK");
        }
        while (icapsock.getLine(buff, 8192, o.content.content_scanner_timeout) > 0) {
            line = buff;

  	    if (line.startsWith("\r")) {
                break;
            } else if (line.startsWith("Preview:")) {
              previewsize = line.after(": ").toInteger();
	      if (previewsize > 0)
	      	usepreviews = true;
            } else if (line.startsWith("Server:")) {
                if (line.contains("AntiVir-WebGate")) {
                    needsBody = true;
                }
            } else if (line.startsWith("Service-ID:")) {
                if (line.contains("KAVIcap")) {
                    needsBody = true;
                }
            } else if (line.startsWith("X-Allow-Out:")) {
                if (line.contains("X-Infection-Found")) {
                    supportsXIF = true;
                }
	    // Dr web bug ICAP response header without suportXIF 
            } else if (line.startsWith("Service: Dr.Web")) {
                    supportsXIF = true;
                }

            e2logger_debugicapc("ICAP/1.0 OPTIONS response part: ", line);
        }
        icapsock.close();
    } catch (std::exception &e) {
        e2logger_error("ICAP server did not respond to OPTIONS request: ", e.what());
        return E2CS_ERROR;
    }

    if (usepreviews){
        e2logger_debugicapc( "Message previews enabled; size: ", previewsize);
    } else {
        e2logger_debugicapc("Message previews enabled; size: disabled");
    }	

    return E2CS_OK;
}

// send memory buffer to ICAP server for scanning
int icapinstance::scanMemory(HTTPHeader *requestheader, HTTPHeader *docheader, const char *user, FOptionContainer* &foc,
    const char *ip, const char *object, unsigned int objectsize, NaughtyFilter *checkme,
    const String *disposition, const String *mimetype)
{
    lastvirusname = lastmessage = "";

    Socket icapsock;

    if (not doHeaders(icapsock, requestheader, docheader, objectsize)) {
        icapsock.close();
        return E2CS_SCANERROR;
    }

    if (usepreviews && (objectsize > previewsize)){
        e2logger_debugicapc("Sending memory date to icap preview first");
    }	

    unsigned int sent = 0;
    if (usepreviews && (objectsize > previewsize)) {
        try {
            if (!icapsock.writeToSocket(object, previewsize, 0, o.content.content_scanner_timeout)) {
                throw std::runtime_error("standard error");
            }
            sent += previewsize;
            icapsock.writeString("\r\n0\r\n\r\n");
            int rc = doScan(icapsock, docheader, object, objectsize, checkme);
            if (rc != ICAP_CONTINUE)
                return rc;
            // some servers send "continue" immediately followed by another response
            if (icapsock.checkForInput()) {
                int rc = doScan(icapsock, docheader, object, objectsize, checkme);
                if (rc != ICAP_NODATA)
                    return rc;
            }
            char objectsizehex[32];
            snprintf(objectsizehex, sizeof(objectsizehex), "%x\r\n", objectsize - previewsize);
            icapsock.writeString(objectsizehex);
        } catch (std::exception &e) {

            e2logger_debugicapc("Exception sending message preview to ICAP: ", e.what());
	        // this *might* just be an early response & closed connection
            if (icapsock.checkForInput()) {
                int rc = doScan(icapsock, docheader, object, objectsize, checkme);
                if (rc != ICAP_NODATA)
                    return rc;
            }
            icapsock.close();
            lastmessage = "Exception sending message preview to ICAP";
            e2logger_error(lastmessage, e.what());
            return E2CS_SCANERROR;
        }
    }
    try {
        if(icapsock.writeToSocket(object + sent, objectsize - sent, 0, o.content.content_scanner_timeout)) {

            e2logger_debugicapc("total sent to icap: ", objectsize);

	        icapsock.writeString("\r\n0\r\n\r\n"); // end marker
            e2logger_debugicapc("memory was sent to icap");

    	}
    } catch (std::exception &e) {

        e2logger_debugicapc("Exception sending memory file to ICAP: ", e.what());

        // this *might* just be an early response & closed connection
        if (icapsock.checkForInput()) {
            int rc = doScan(icapsock, docheader, object, objectsize, checkme);
            if (rc != ICAP_NODATA)
                return rc;
        }
        icapsock.close();
        lastmessage = "Exception sending memory file to ICAP";
        e2logger_error(lastmessage, e.what());
        return E2CS_SCANERROR;
    }

    return doScan(icapsock, docheader, object, objectsize, checkme);
}

// send file contents for scanning
int icapinstance::scanFile(HTTPHeader *requestheader, HTTPHeader *docheader, const char *user,
    FOptionContainer* &foc, const char *ip, const char *filename, NaughtyFilter *checkme,
    const String *disposition, const String *mimetype)
{
    lastmessage = lastvirusname = "";
    int filefd = open(filename, O_RDONLY);
    if (filefd < 0) {

        e2logger_debugicapc("Error opening file (", filename, "): ", strerror(errno));

	    lastmessage = "Error opening file to send to ICAP";
        e2logger_error(lastmessage, strerror(errno));
        return E2CS_SCANERROR;
    }
    lseek(filefd, 0, SEEK_SET);
    unsigned int filesize = lseek(filefd, 0, SEEK_END);

    Socket icapsock;
    if (not doHeaders(icapsock, requestheader, docheader, filesize)) {
        icapsock.close();
        close(filefd);
        return E2CS_SCANERROR;
    }

    lseek(filefd, 0, SEEK_SET);
    unsigned int sent = 0;
    char *data = new char[previewsize];
    char *object = new char[100];
    int objectsize = 0;

    e2logger_debugicapc("About to send file data to icap");

    if (usepreviews && (filesize > previewsize)) {
        try {
            while (sent < previewsize) {
                int rc = read(filefd, data, previewsize);
                if (rc < 0) {
                    throw std::runtime_error("could not read from file");
                }
                if (rc == 0) {
                    break; // should never happen
                }
                if (!icapsock.writeToSocket(data, rc, 0, o.content.content_scanner_timeout)) {
                    throw std::runtime_error("could not write to socket");
                }
                memcpy(object, data, (rc > 100) ? 100 : rc);
                objectsize += (rc > 100) ? 100 : rc;
                sent += rc;
            }
            icapsock.writeString("\r\n0\r\n\r\n");
            int rc = doScan(icapsock, docheader, object, objectsize, checkme);
            if (rc != ICAP_CONTINUE) {
                delete[] data;
                close(filefd);
                return rc;
            }
            // some servers send "continue" immediately followed by another response
            if (icapsock.checkForInput()) {
                int rc = doScan(icapsock, docheader, object, objectsize, checkme);
                if (rc != ICAP_NODATA) {
                    delete[] data;
                    close(filefd);
                    return rc;
                }
            }
            char objectsizehex[32];
            snprintf(objectsizehex, sizeof(objectsizehex), "%x\r\n", filesize - previewsize);
            icapsock.writeString(objectsizehex);
        } catch (std::exception &e) {

    	    icapsock.close();
            lastmessage = "Exception sending message preview to ICAP";
            e2logger_error(lastmessage, e.what());
            e2logger_debugicapc(lastmessage, e.what());
            delete[] data;
            close(filefd);
            // this *might* just be an early response & closed connection
            if (icapsock.checkForInput()) {
                int rc = doScan(icapsock, docheader, object, objectsize, checkme);
                if (rc != ICAP_NODATA)
                    return rc;
            }
            return E2CS_SCANERROR;
        }
    }

    delete[] data;
    data = new char[256 * 1024]; // 256k

    try {
        while (sent < filesize) {
            int rc = read(filefd, data, 256 * 1024);
            e2logger_debugicapc("reading icap file rc: ", rc);
            if (rc < 0) {
            	e2logger_debugicapc("error reading icap file so throwing exception");
                throw std::runtime_error("could not read from file");
            }
            if (rc == 0) {
                e2logger_debugicapc("got zero bytes reading icap file");
                break; // should never happen
            }
            memcpy(object + objectsize, data, (rc > (100 - objectsize)) ? (100 - objectsize) : rc);
            objectsize += (rc > (100 - objectsize)) ? (100 - objectsize) : rc;
            if (!icapsock.writeToSocket(data, rc, 0, o.content.content_scanner_timeout)) {
                throw std::runtime_error("Unable to write to socket");
            };
            sent += rc;
        }

        e2logger_debugicapc("total sent to icap: ", sent);

        icapsock.writeString("\r\n0\r\n\r\n"); // end marker

        e2logger_debugicapc("file was sent to icap");

    } catch (std::exception &e) {

        e2logger_debugicapc( "Exception sending file to ICAP: ", e.what());
        lastmessage = "Exception sending file to ICAP";
        e2logger_error(lastmessage, e.what());
        delete[] data;
        close(filefd);
        // this *might* just be an early response & closed connection
        if (icapsock.checkForInput()) {
            int rc = doScan(icapsock, docheader, object, objectsize, checkme);
            if (rc != ICAP_NODATA)
                return rc;
        }
        return E2CS_SCANERROR;
    }
    close(filefd);
    delete[] data;
    return doScan(icapsock, docheader, object, objectsize, checkme);
}

// send ICAP request headers, returning success or failure
bool icapinstance::doHeaders(Socket &icapsock, HTTPHeader *reqheader, HTTPHeader *respheader, unsigned int objectsize)
{
    int rc = icapsock.connect(icapip.toCharArray(), icapport);
    if (rc) {
	    lastmessage = "Error connecting to ICAP server";
        e2logger_debugicapc(lastmessage);
        e2logger_error(lastmessage);
        return false;
    }
    char objectsizehex[32];
    // encapsulated HTTP request header:
    // use a dummy unless it proves absolutely necessary to do otherwise,
    // as using real data could lead to e.g. yet another source of password
    // leakage over the network.
    String encapsulatedheader("GET " + reqheader->getUrl() + " HTTP/1.0\r\n\r\n");
    // body chunk size in hex - either full body, or just preview
    if (usepreviews && (objectsize > previewsize)) {
        snprintf(objectsizehex, sizeof(objectsizehex), "%x\r\n", previewsize);
    } else {
        snprintf(objectsizehex, sizeof(objectsizehex), "%x\r\n", objectsize);
    }
    // encapsulated HTTP response header:
    // use real data, because scanners can use this to aid the process
    /*String httpresponseheader;
	for (std::deque<String>::iterator i = respheader->header.begin(); i != respheader->header.end(); i++) {
		httpresponseheader += (*i) + "\r\n";
	}
	httpresponseheader += "\r\n";*/
    String httpresponseheader("HTTP/1.0 200 OK\r\n\r\n");
    // ICAP header itself
    String icapheader("RESPMOD " + icapurl + " ICAP/1.0\r\nHost: " + icaphost + "\r\nAllow: 204\r\nEncapsulated: req-hdr=0, res-hdr=" + String(encapsulatedheader.length()) + ", res-body=" + String(httpresponseheader.length() + encapsulatedheader.length()));
    if (usepreviews && (objectsize > previewsize)) {
        icapheader += "\r\nPreview: " + String(previewsize);
    }
    icapheader += "\r\n\r\n";

    e2logger_debugicapc("About to send icapheader:\n", icapheader, encapsulatedheader, httpresponseheader, objectsizehex);

    try {
        icapsock.writeString(icapheader.toCharArray());
        icapsock.writeString(encapsulatedheader.toCharArray());
        icapsock.writeString(httpresponseheader.toCharArray());
        icapsock.writeString(objectsizehex);
    } catch (std::exception &e) {
        
	    lastmessage = "Exception sending headers to ICAP";
        e2logger_debugicapc(lastmessage, e.what());
        e2logger_error(lastmessage, e.what());
        return false;
    }
    return true;
}

// check data received from ICAP server and interpret as virus name & return value
int icapinstance::doScan(Socket &icapsock, HTTPHeader *docheader, const char *object, unsigned int objectsize, NaughtyFilter *checkme)
{
    char *data = new char[8192];
    try {
        String line;
        int rc = icapsock.getLine(data, 8192, o.content.content_scanner_timeout);
        if (rc == 0)
            return ICAP_NODATA;
        line = data;
        e2logger_debugicapc("reply from icap: ", line);

	    // reply is of the format:
        // ICAP/1.0 204 No Content Necessary (etc)

        String returncode(line.after(" ").before(" "));

        if (returncode == "204") {
            e2logger_debugicapc("ICAP says clean!");
	        delete[] data;
           return E2CS_CLEAN;
        } else if (returncode == "100") {

            e2logger_debugicapc("ICAP says continue!");

            // discard rest of headers (usually just a blank line)
            // this is so we are in the right place in the data stream to
            // call doScan() again later, because people like Symantec seem
            // to think sending code 100 then code 204 one after the other
            // is not an abuse of the ICAP specification.
            while (icapsock.getLine(data, 8192, o.content.content_scanner_timeout) > 0) {
                if (data[0] == 13)
                    break;
            }
            delete[] data;
            return ICAP_CONTINUE;
        } else if (returncode == "200") {

            e2logger_debugicapc("ICAP says maybe not clean!");

	        while (icapsock.getLine(data, 8192, o.content.content_scanner_timeout) > 0) {
                if (data[0] == 13) // end marker
                    break;
                line = data;
                // Symantec's engine gives us the virus name in the ICAP headers
                if (supportsXIF && line.startsWith("X-Infection-Found")) {
                    e2logger_debugicapc("ICAP says infected! (X-Infection-Found)");
		            lastvirusname = line.after("Threat=").before(";");
                    delete[] data;

                    blockFile(NULL, NULL, checkme);
                    return E2CS_INFECTED;
                }
            }
            // AVIRA's and KAV Antivir gives us 200 in all cases, so
            // - unfortunately - we must pay attention to the encapsulated
            // header/body.
            if (needsBody) {
                // grab & compare the HTTP return code from modified response
                // if it's been modified, assume there's an infection
                icapsock.getLine(data, 8192, o.content.content_scanner_timeout);
                line = data;

                e2logger_debugicapc( "Comparing original return code to modified:", docheader->header.front(), " ", line);

		        int respmodReturnCode = line.after(" ").before(" ").toInteger();
                if (respmodReturnCode != docheader->returnCode()) {

                    e2logger_debugicapc("ICAP says infected! (returned header comparison)");

		            delete[] data;
                    lastvirusname = "Unknown";

                    blockFile(NULL, NULL, checkme);
                    return E2CS_INFECTED;
                }
                // ok - headers were identical, so look at encapsulated body
                // discard the rest of the encapsulated headers
                while (icapsock.getLine(data, 8192, o.content.content_scanner_timeout) > 0) {
                    if (data[0] == 13)
                        break;
                }
                // grab body chunk size
		        icapsock.getLine(data, 8192, o.content.content_scanner_timeout);
                line = data;

                e2logger_debugicapc("Comparing original body data to modified");

                int bodysize = line.hexToInteger();
                // get, say, the first 100 bytes and compare them to what we
                // originally sent to see if it has been modified
                unsigned int chunksize = (bodysize < 100) ? bodysize : 100;
                if (chunksize > objectsize)
                    chunksize = objectsize;
                icapsock.readFromSocket(data, chunksize, 0, o.content.content_scanner_timeout);
                if (memcmp(data, object, chunksize) == 0) {

                    e2logger_debugicapc("ICAP says clean! (body byte comparison)");

                    delete[] data;
                    return E2CS_CLEAN;
                } else {

                   	e2logger_debugicapc("ICAP says infected! (body byte comparison)");

		            delete[] data;
                    lastvirusname = "Unknown";

                    blockFile(NULL, NULL, checkme);
                    return E2CS_INFECTED;
                }
            }
            // even if we don't find an X-Infection-Found header,
            // the file is still infected!

            e2logger_debugicapc("ICAP says infected! (no further tests)");

	    delete[] data;
            lastvirusname = "Unknown";

            blockFile(NULL, NULL, checkme);
            return E2CS_INFECTED;
        } else if (returncode == "404") {

            e2logger_debugicapc("ICAP says no such service!");

	        lastmessage = "ICAP reports no such service";
            e2logger_error(lastmessage, " check your server URL");
            delete[] data;
            return E2CS_SCANERROR;
        } else {

	        lastmessage = "ICAP returned unrecognised response code.";
            e2logger_debugicapc(lastmessage, returncode);
            e2logger_error(lastmessage, returncode);
            delete[] data;
            return E2CS_SCANERROR;
        }
        delete[] data;
    } catch (std::exception &e) {

        lastmessage = "Exception getting reply from ICAP.";
        e2logger_error(lastmessage, e.what());
        e2logger_debugicapc(lastmessage, e.what());
        delete[] data;
        return E2CS_SCANERROR;
    }
    // it is generally NOT a good idea, when using virus scanning,
    // to continue as if nothing went wrong by default!
    return E2CS_SCANERROR;
}
