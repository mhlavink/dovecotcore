/* Copyright (c) 2017-2023 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "randgen.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "iostream-temp.h"
#include "iostream-pump.h"
#include "test-common.h"

#include "json-parser.new.h"
#include "json-generator.h"

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

static bool debug = FALSE;

struct json_io_test {
	const char *input;
	const char *output;
	struct json_limits limits;
	enum json_parser_flags flags;
};

static const struct json_io_test
tests[] = {
	{
		.input = "123456789"
	},{
		.input = "\"frop\""
	},{
		.input = "false"
	},{
		.input = "null"
	},{
		.input = "true"
	},{
		.input = "[]"
	},{
		.input = "[[]]"
	},{
		.input = "[[[[[[[[[[[[]]]]]]]]]]]]"
	},{
		.input = "[[],[],[]]"
	},{
		.input = "[[[],[],[]],[[],[],[]],[[],[],[]]]"
	},{
		.input = "{}"
	},{
		.input = "[\"frop\"]"
	},{
		.input = "[\"frop\",\"friep\"]"
	},{
		.input = "[\"frop\",\"friep\",\"frml\"]"
	},{
		.input = "[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\"]"
	},{
		.input = "[true]"
	},{
		.input = "[null]"
	},{
		.input = "[true,false]"
	},{
		.input = "[true,true,false,false]"
	},{
		.input = "[1]"
	},{
		.input = "[1,12]"
	},{
		.input = "[1,12,123]"
	},{
		.input = "[1,12,123,1234]"
	},{
		.input = "[1,2,3,4,5,6,7]"
	},{
		.input = "{\"frop\":1}"
	},{
		.input = "{\"a\":\"1\",\"b\":\"2\",\"c\":\"3\",\"d\":\"4\",\"e\":\"5\",\"f\":\"6\",\"g\":\"7\"}"
	},{
		.input = "[{\"frop\":1},{\"frop\":1},{\"frop\":1},{\"frop\":1},{\"frop\":1}]"
	},{
		.input = "[[\"frop\",1],[\"frop\",1],[\"frop\",1],[\"frop\",1],[\"frop\",1]]"
	},{
		.input = "[[\"frop\",[]],[\"frop\",[]],[\"frop\",[]],[\"frop\",[]],[\"frop\",[]]]"
	},{
		.input = "[[\"frop\"],[1],[\"frop\"],[1],[\"frop\"],[1],[\"frop\"],[1],[\"frop\"],[1]]"
	},{
		.input = "[[\"frop\"],[1,2,false],[\"frop\"],[1,2,false],[\"frop\"],[1,2,false],[\"frop\"],[1,2,false],[\"frop\"],[1,2,false]]"
	},{
		.input = "[[\"frop\",{}],[\"frop\",{}],[\"frop\",{}],[\"frop\",{}],[\"frop\",{}]]"
	},{
		.input = "{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":{}}}}}}}}}"
	},{
		.input =
			"{\n"
			"    \"glossary\": {\n"
			"        \"title\": \"example glossary\",\n"
			"		\"GlossDiv\": {\n"
			"            \"title\": \"S\",\n"
			"			\"GlossList\": {\n"
			"                \"GlossEntry\": {\n"
			"                    \"ID\": \"SGML\",\n"
			"					\"SortAs\": \"SGML\",\n"
			"					\"GlossTerm\": \"Standard Generalized Markup Language\",\n"
			"					\"Acronym\": \"SGML\",\n"
			"					\"Abbrev\": \"ISO 8879:1986\",\n"
			"					\"GlossDef\": {\n"
			"                        \"para\": \"A meta-markup language, used to create markup languages such as DocBook.\",\n"
			"						\"GlossSeeAlso\": [\"GML\", \"XML\"]\n"
			"                    },\n"
			"					\"GlossSee\": \"markup\"\n"
			"                }\n"
			"            }\n"
			"        }\n"
			"    }\n"
			"}\n",
		.output =
			"{\"glossary\":{\"title\":\"example glossary\",\"GlossDiv\":{"
			"\"title\":\"S\",\"GlossList\":{\"GlossEntry\":{\"ID\":\"SGML\","
			"\"SortAs\":\"SGML\",\"GlossTerm\":\"Standard Generalized Markup Language\","
			"\"Acronym\":\"SGML\",\"Abbrev\":\"ISO 8879:1986\",\"GlossDef\":{"
			"\"para\":\"A meta-markup language, used to create markup languages such as DocBook.\","
			"\"GlossSeeAlso\":[\"GML\",\"XML\"]},\"GlossSee\":\"markup\"}}}}}"
	},{
		.input =
			"{\"menu\": {\n"
			"  \"id\": \"file\",\n"
			"  \"value\": \"File\",\n"
			"  \"popup\": {\n"
			"    \"menuitem\": [\n"
			"      {\"value\": \"New\", \"onclick\": \"CreateNewDoc()\"},\n"
			"      {\"value\": \"Open\", \"onclick\": \"OpenDoc()\"},\n"
			"      {\"value\": \"Close\", \"onclick\": \"CloseDoc()\"}\n"
			"    ]\n"
			"  }\n"
			"}}\n",
		.output =
			"{\"menu\":{\"id\":\"file\",\"value\":\"File\","
			"\"popup\":{\"menuitem\":[{\"value\":\"New\",\"onclick\":"
			"\"CreateNewDoc()\"},{\"value\":\"Open\",\"onclick\":\"OpenDoc()\"},"
			"{\"value\":\"Close\",\"onclick\":\"CloseDoc()\"}]}}}"
	},{
		.input =
			"{\"widget\": {\n"
			"    \"debug\": \"on\",\n"
			"    \"window\": {\n"
			"        \"title\": \"Sample Konfabulator Widget\",\n"
			"        \"name\": \"main_window\",\n"
			"        \"width\": 500,\n"
			"        \"height\": 500\n"
			"    },\n"
			"    \"image\": { \n"
			"        \"src\": \"Images/Sun.png\",\n"
			"        \"name\": \"sun1\",\n"
			"        \"hOffset\": 250,\n"
			"        \"vOffset\": 250,\n"
			"        \"alignment\": \"center\"\n"
			"    },\n"
			"    \"text\": {\n"
			"        \"data\": \"Click Here\",\n"
			"        \"size\": 36,\n"
			"        \"style\": \"bold\",\n"
			"        \"name\": \"text1\",\n"
			"        \"hOffset\": 250,\n"
			"        \"vOffset\": 100,\n"
			"        \"alignment\": \"center\",\n"
			"        \"onMouseUp\": \"sun1.opacity = (sun1.opacity / 100) * 90;\"\n"
			"    }\n"
			"}}\n",
		.output =
			"{\"widget\":{\"debug\":\"on\",\"window\":{"
			"\"title\":\"Sample Konfabulator Widget\","
			"\"name\":\"main_window\",\"width\":500,"
			"\"height\":500},\"image\":{\"src\":\"Images/Sun.png\","
			"\"name\":\"sun1\",\"hOffset\":250,\"vOffset\":250,"
			"\"alignment\":\"center\"},\"text\":{\"data\":\"Click Here\","
			"\"size\":36,\"style\":\"bold\",\"name\":\"text1\","
			"\"hOffset\":250,\"vOffset\":100,\"alignment\":\"center\","
			"\"onMouseUp\":\"sun1.opacity = (sun1.opacity / 100) * 90;\"}}}"
	},{
		.input =
			"{\"web-app\": {\r\n"
			"  \"servlet\": [   \r\n"
			"    {\r\n"
			"      \"servlet-name\": \"cofaxCDS\",\r\n"
			"      \"servlet-class\": \"org.cofax.cds.CDSServlet\",\r\n"
			"      \"init-param\": {\r\n"
			"        \"configGlossary:installationAt\": \"Philadelphia, PA\",\r\n"
			"        \"configGlossary:adminEmail\": \"ksm@pobox.com\",\r\n"
			"        \"configGlossary:poweredBy\": \"Cofax\",\r\n"
			"        \"configGlossary:poweredByIcon\": \"/images/cofax.gif\",\r\n"
			"        \"configGlossary:staticPath\": \"/content/static\",\r\n"
			"        \"templateProcessorClass\": \"org.cofax.WysiwygTemplate\",\r\n"
			"        \"templateLoaderClass\": \"org.cofax.FilesTemplateLoader\",\r\n"
			"        \"templatePath\": \"templates\",\r\n"
			"        \"templateOverridePath\": \"\",\r\n"
			"        \"defaultListTemplate\": \"listTemplate.htm\",\r\n"
			"        \"defaultFileTemplate\": \"articleTemplate.htm\",\r\n"
			"        \"useJSP\": false,\r\n"
			"        \"jspListTemplate\": \"listTemplate.jsp\",\r\n"
			"        \"jspFileTemplate\": \"articleTemplate.jsp\",\r\n"
			"        \"cachePackageTagsTrack\": 200,\r\n"
			"        \"cachePackageTagsStore\": 200,\r\n"
			"        \"cachePackageTagsRefresh\": 60,\r\n"
			"        \"cacheTemplatesTrack\": 100,\r\n"
			"        \"cacheTemplatesStore\": 50,\r\n"
			"        \"cacheTemplatesRefresh\": 15,\r\n"
			"        \"cachePagesTrack\": 200,\r\n"
			"        \"cachePagesStore\": 100,\r\n"
			"        \"cachePagesRefresh\": 10,\r\n"
			"        \"cachePagesDirtyRead\": 10,\r\n"
			"        \"searchEngineListTemplate\": \"forSearchEnginesList.htm\",\r\n"
			"        \"searchEngineFileTemplate\": \"forSearchEngines.htm\",\r\n"
			"        \"searchEngineRobotsDb\": \"WEB-INF/robots.db\",\r\n"
			"        \"useDataStore\": true,\r\n"
			"        \"dataStoreClass\": \"org.cofax.SqlDataStore\",\r\n"
			"        \"redirectionClass\": \"org.cofax.SqlRedirection\",\r\n"
			"        \"dataStoreName\": \"cofax\",\r\n"
			"        \"dataStoreDriver\": \"com.microsoft.jdbc.sqlserver.SQLServerDriver\",\r\n"
			"        \"dataStoreUrl\": \"jdbc:microsoft:sqlserver://LOCALHOST:1433;DatabaseName=goon\",\r\n"
			"        \"dataStoreUser\": \"sa\",\r\n"
			"        \"dataStorePassword\": \"dataStoreTestQuery\",\r\n"
			"        \"dataStoreTestQuery\": \"SET NOCOUNT ON;select test='test';\",\r\n"
			"        \"dataStoreLogFile\": \"/usr/local/tomcat/logs/datastore.log\",\r\n"
			"        \"dataStoreInitConns\": 10,\r\n"
			"        \"dataStoreMaxConns\": 100,\r\n"
			"        \"dataStoreConnUsageLimit\": 100,\r\n"
			"        \"dataStoreLogLevel\": \"debug\",\r\n"
			"        \"maxUrlLength\": 500}},\r\n"
			"    {\r\n"
			"      \"servlet-name\": \"cofaxEmail\",\r\n"
			"      \"servlet-class\": \"org.cofax.cds.EmailServlet\",\r\n"
			"      \"init-param\": {\r\n"
			"      \"mailHost\": \"mail1\",\r\n"
			"      \"mailHostOverride\": \"mail2\"}},\r\n"
			"    {\r\n"
			"      \"servlet-name\": \"cofaxAdmin\",\r\n"
			"      \"servlet-class\": \"org.cofax.cds.AdminServlet\"},\r\n"
			" \r\n"
			"    {\r\n"
			"      \"servlet-name\": \"fileServlet\",\r\n"
			"      \"servlet-class\": \"org.cofax.cds.FileServlet\"},\r\n"
			"    {\r\n"
			"      \"servlet-name\": \"cofaxTools\",\r\n"
			"      \"servlet-class\": \"org.cofax.cms.CofaxToolsServlet\",\r\n"
			"      \"init-param\": {\r\n"
			"        \"templatePath\": \"toolstemplates/\",\r\n"
			"        \"log\": 1,\r\n"
			"        \"logLocation\": \"/usr/local/tomcat/logs/CofaxTools.log\",\r\n"
			"        \"logMaxSize\": \"\",\r\n"
			"        \"dataLog\": 1,\r\n"
			"        \"dataLogLocation\": \"/usr/local/tomcat/logs/dataLog.log\",\r\n"
			"        \"dataLogMaxSize\": \"\",\r\n"
			"        \"removePageCache\": \"/content/admin/remove?cache=pages&id=\",\r\n"
			"        \"removeTemplateCache\": \"/content/admin/remove?cache=templates&id=\",\r\n"
			"        \"fileTransferFolder\": \"/usr/local/tomcat/webapps/content/fileTransferFolder\",\r\n"
			"        \"lookInContext\": 1,\r\n"
			"        \"adminGroupID\": 4,\r\n"
			"        \"betaServer\": true}}],\r\n"
			"  \"servlet-mapping\": {\r\n"
			"    \"cofaxCDS\": \"/\",\r\n"
			"    \"cofaxEmail\": \"/cofaxutil/aemail/*\",\r\n"
			"    \"cofaxAdmin\": \"/admin/*\",\r\n"
			"    \"fileServlet\": \"/static/*\",\r\n"
			"    \"cofaxTools\": \"/tools/*\"},\r\n"
			" \r\n"
			"  \"taglib\": {\r\n"
			"    \"taglib-uri\": \"cofax.tld\",\r\n"
			"    \"taglib-location\": \"/WEB-INF/tlds/cofax.tld\"}}}",
		.output =
			"{\"web-app\":{\"servlet\":[{\"servlet-name\":\"cofaxCDS\","
			"\"servlet-class\":\"org.cofax.cds.CDSServlet\","
			"\"init-param\":{\"configGlossary:installationAt\":\"Philadelphia, PA\","
			"\"configGlossary:adminEmail\":\"ksm@pobox.com\","
			"\"configGlossary:poweredBy\":\"Cofax\","
			"\"configGlossary:poweredByIcon\":\"/images/cofax.gif\","
			"\"configGlossary:staticPath\":\"/content/static\","
			"\"templateProcessorClass\":\"org.cofax.WysiwygTemplate\","
			"\"templateLoaderClass\":\"org.cofax.FilesTemplateLoader\","
			"\"templatePath\":\"templates\","
			"\"templateOverridePath\":\"\","
			"\"defaultListTemplate\":\"listTemplate.htm\","
			"\"defaultFileTemplate\":\"articleTemplate.htm\","
			"\"useJSP\":false,\"jspListTemplate\":\"listTemplate.jsp\","
			"\"jspFileTemplate\":\"articleTemplate.jsp\","
			"\"cachePackageTagsTrack\":200,\"cachePackageTagsStore\":200,"
			"\"cachePackageTagsRefresh\":60,\"cacheTemplatesTrack\":100,"
			"\"cacheTemplatesStore\":50,\"cacheTemplatesRefresh\":15,"
			"\"cachePagesTrack\":200,\"cachePagesStore\":100,"
			"\"cachePagesRefresh\":10,\"cachePagesDirtyRead\":10,"
			"\"searchEngineListTemplate\":\"forSearchEnginesList.htm\","
			"\"searchEngineFileTemplate\":\"forSearchEngines.htm\","
			"\"searchEngineRobotsDb\":\"WEB-INF/robots.db\","
			"\"useDataStore\":true,\"dataStoreClass\":\"org.cofax.SqlDataStore\","
			"\"redirectionClass\":\"org.cofax.SqlRedirection\","
			"\"dataStoreName\":\"cofax\","
			"\"dataStoreDriver\":\"com.microsoft.jdbc.sqlserver.SQLServerDriver\","
			"\"dataStoreUrl\":\"jdbc:microsoft:sqlserver://LOCALHOST:1433;DatabaseName=goon\","
			"\"dataStoreUser\":\"sa\",\"dataStorePassword\":\"dataStoreTestQuery\","
			"\"dataStoreTestQuery\":\"SET NOCOUNT ON;select test='test';\","
			"\"dataStoreLogFile\":\"/usr/local/tomcat/logs/datastore.log\","
			"\"dataStoreInitConns\":10,\"dataStoreMaxConns\":100,"
			"\"dataStoreConnUsageLimit\":100,\"dataStoreLogLevel\":\"debug\","
			"\"maxUrlLength\":500}},{\"servlet-name\":\"cofaxEmail\","
			"\"servlet-class\":\"org.cofax.cds.EmailServlet\","
			"\"init-param\":{\"mailHost\":\"mail1\",\"mailHostOverride\":\"mail2\"}},"
			"{\"servlet-name\":\"cofaxAdmin\","
			"\"servlet-class\":\"org.cofax.cds.AdminServlet\"},{"
			"\"servlet-name\":\"fileServlet\","
			"\"servlet-class\":\"org.cofax.cds.FileServlet\"},{"
			"\"servlet-name\":\"cofaxTools\","
			"\"servlet-class\":\"org.cofax.cms.CofaxToolsServlet\","
			"\"init-param\":{\"templatePath\":\"toolstemplates/\","
			"\"log\":1,\"logLocation\":\"/usr/local/tomcat/logs/CofaxTools.log\","
			"\"logMaxSize\":\"\",\"dataLog\":1,"
			"\"dataLogLocation\":\"/usr/local/tomcat/logs/dataLog.log\","
			"\"dataLogMaxSize\":\"\","
			"\"removePageCache\":\"/content/admin/remove?cache=pages&id=\","
			"\"removeTemplateCache\":\"/content/admin/remove?cache=templates&id=\","
			"\"fileTransferFolder\":\"/usr/local/tomcat/webapps/content/fileTransferFolder\","
			"\"lookInContext\":1,\"adminGroupID\":4,\"betaServer\":true}}],"
			"\"servlet-mapping\":{\"cofaxCDS\":\"/\","
			"\"cofaxEmail\":\"/cofaxutil/aemail/*\","
			"\"cofaxAdmin\":\"/admin/*\",\"fileServlet\":\"/static/*\","
			"\"cofaxTools\":\"/tools/*\"},\"taglib\":{"
			"\"taglib-uri\":\"cofax.tld\","
			"\"taglib-location\":\"/WEB-INF/tlds/cofax.tld\"}}}"
	},{
		.input =
			"{\"menu\": {\r\n"
			"    \"header\": \"SVG Viewer\",\r\n"
			"    \"items\": [\r\n"
			"        {\"id\": \"Open\"},\r\n"
			"        {\"id\": \"OpenNew\", \"label\": \"Open New\"},\r\n"
			"        null,\r\n"
			"        {\"id\": \"ZoomIn\", \"label\": \"Zoom In\"},\r\n"
			"        {\"id\": \"ZoomOut\", \"label\": \"Zoom Out\"},\r\n"
			"        {\"id\": \"OriginalView\", \"label\": \"Original View\"},\r\n"
			"        null,\r\n"
			"        {\"id\": \"Quality\"},\r\n"
			"        {\"id\": \"Pause\"},\r\n"
			"        {\"id\": \"Mute\"},\r\n"
			"        null,\r\n"
			"        {\"id\": \"Find\", \"label\": \"Find...\"},\r\n"
			"        {\"id\": \"FindAgain\", \"label\": \"Find Again\"},\r\n"
			"        {\"id\": \"Copy\"},\r\n"
			"        {\"id\": \"CopyAgain\", \"label\": \"Copy Again\"},\r\n"
			"        {\"id\": \"CopySVG\", \"label\": \"Copy SVG\"},\r\n"
			"        {\"id\": \"ViewSVG\", \"label\": \"View SVG\"},\r\n"
			"        {\"id\": \"ViewSource\", \"label\": \"View Source\"},\r\n"
			"        {\"id\": \"SaveAs\", \"label\": \"Save As\"},\r\n"
			"        null,\r\n"
			"        {\"id\": \"Help\"},\r\n"
			"        {\"id\": \"About\", \"label\": \"About Adobe CVG Viewer...\"}\r\n"
			"    ]\r\n"
			"}}",
		.output =
			"{\"menu\":{\"header\":\"SVG Viewer\",\"items\":["
			"{\"id\":\"Open\"},{\"id\":\"OpenNew\",\"label\":\"Open New\"},"
			"null,{\"id\":\"ZoomIn\",\"label\":\"Zoom In\"},"
			"{\"id\":\"ZoomOut\",\"label\":\"Zoom Out\"},"
			"{\"id\":\"OriginalView\",\"label\":\"Original View\"},"
			"null,{\"id\":\"Quality\"},{\"id\":\"Pause\"},"
			"{\"id\":\"Mute\"},null,{\"id\":\"Find\",\"label\":\"Find...\"},"
			"{\"id\":\"FindAgain\",\"label\":\"Find Again\"},"
			"{\"id\":\"Copy\"},{\"id\":\"CopyAgain\",\"label\":\"Copy Again\"},"
			"{\"id\":\"CopySVG\",\"label\":\"Copy SVG\"},"
			"{\"id\":\"ViewSVG\",\"label\":\"View SVG\"},"
			"{\"id\":\"ViewSource\",\"label\":\"View Source\"},"
			"{\"id\":\"SaveAs\",\"label\":\"Save As\"},"
			"null,{\"id\":\"Help\"},"
			"{\"id\":\"About\",\"label\":\"About Adobe CVG Viewer...\"}]}}"
	},{
		.input =
			"{\r\n"
			"    \"$schema\": \"http://json-schema.org/draft-06/schema#\",\r\n"
			"    \"$id\": \"http://json-schema.org/draft-06/schema#\",\r\n"
			"    \"title\": \"Core schema meta-schema\",\r\n"
			"    \"definitions\": {\r\n"
			"        \"schemaArray\": {\r\n"
			"            \"type\": \"array\",\r\n"
			"            \"minItems\": 1,\r\n"
			"            \"items\": { \"$ref\": \"#\" }\r\n"
			"        },\r\n"
			"        \"nonNegativeInteger\": {\r\n"
			"            \"type\": \"integer\",\r\n"
			"            \"minimum\": 0\r\n"
			"        },\r\n"
			"        \"nonNegativeIntegerDefault0\": {\r\n"
			"            \"allOf\": [\r\n"
			"                { \"$ref\": \"#/definitions/nonNegativeInteger\" },\r\n"
			"                { \"default\": 0 }\r\n"
			"            ]\r\n"
			"        },\r\n"
			"        \"simpleTypes\": {\r\n"
			"            \"enum\": [\r\n"
			"                \"array\",\r\n"
			"                \"boolean\",\r\n"
			"                \"integer\",\r\n"
			"                \"null\",\r\n"
			"                \"number\",\r\n"
			"                \"object\",\r\n"
			"                \"string\"\r\n"
			"            ]\r\n"
			"        },\r\n"
			"        \"stringArray\": {\r\n"
			"            \"type\": \"array\",\r\n"
			"            \"items\": { \"type\": \"string\" },\r\n"
			"            \"uniqueItems\": true,\r\n"
			"            \"default\": []\r\n"
			"        }\r\n"
			"    },\r\n"
			"    \"type\": [\"object\", \"boolean\"],\r\n"
			"    \"properties\": {\r\n"
			"        \"$id\": {\r\n"
			"            \"type\": \"string\",\r\n"
			"            \"format\": \"uri-reference\"\r\n"
			"        },\r\n"
			"        \"$schema\": {\r\n"
			"            \"type\": \"string\",\r\n"
			"            \"format\": \"uri\"\r\n"
			"        },\r\n"
			"        \"$ref\": {\r\n"
			"            \"type\": \"string\",\r\n"
			"            \"format\": \"uri-reference\"\r\n"
			"        },\r\n"
			"        \"title\": {\r\n"
			"            \"type\": \"string\"\r\n"
			"        },\r\n"
			"        \"description\": {\r\n"
			"            \"type\": \"string\"\r\n"
			"        },\r\n"
			"        \"default\": {},\r\n"
			"        \"multipleOf\": {\r\n"
			"            \"type\": \"number\",\r\n"
			"            \"exclusiveMinimum\": 0\r\n"
			"        },\r\n"
			"        \"maximum\": {\r\n"
			"            \"type\": \"number\"\r\n"
			"        },\r\n"
			"        \"exclusiveMaximum\": {\r\n"
			"            \"type\": \"number\"\r\n"
			"        },\r\n"
			"        \"minimum\": {\r\n"
			"            \"type\": \"number\"\r\n"
			"        },\r\n"
			"        \"exclusiveMinimum\": {\r\n"
			"            \"type\": \"number\"\r\n"
			"        },\r\n"
			"        \"maxLength\": { \"$ref\": \"#/definitions/nonNegativeInteger\" },\r\n"
			"        \"minLength\": { \"$ref\": \"#/definitions/nonNegativeIntegerDefault0\" },\r\n"
			"        \"pattern\": {\r\n"
			"            \"type\": \"string\",\r\n"
			"            \"format\": \"regex\"\r\n"
			"        },\r\n"
			"        \"additionalItems\": { \"$ref\": \"#\" },\r\n"
			"        \"items\": {\r\n"
			"            \"anyOf\": [\r\n"
			"                { \"$ref\": \"#\" },\r\n"
			"                { \"$ref\": \"#/definitions/schemaArray\" }\r\n"
			"            ],\r\n"
			"            \"default\": {}\r\n"
			"        },\r\n"
			"        \"maxItems\": { \"$ref\": \"#/definitions/nonNegativeInteger\" },\r\n"
			"        \"minItems\": { \"$ref\": \"#/definitions/nonNegativeIntegerDefault0\" },\r\n"
			"        \"uniqueItems\": {\r\n"
			"            \"type\": \"boolean\",\r\n"
			"            \"default\": false\r\n"
			"        },\r\n"
			"        \"contains\": { \"$ref\": \"#\" },\r\n"
			"        \"maxProperties\": { \"$ref\": \"#/definitions/nonNegativeInteger\" },\r\n"
			"        \"minProperties\": { \"$ref\": \"#/definitions/nonNegativeIntegerDefault0\" },\r\n"
			"        \"required\": { \"$ref\": \"#/definitions/stringArray\" },\r\n"
			"        \"additionalProperties\": { \"$ref\": \"#\" },\r\n"
			"        \"definitions\": {\r\n"
			"            \"type\": \"object\",\r\n"
			"            \"additionalProperties\": { \"$ref\": \"#\" },\r\n"
			"            \"default\": {}\r\n"
			"        },\r\n"
			"        \"properties\": {\r\n"
			"            \"type\": \"object\",\r\n"
			"            \"additionalProperties\": { \"$ref\": \"#\" },\r\n"
			"            \"default\": {}\r\n"
			"        },\r\n"
			"        \"patternProperties\": {\r\n"
			"            \"type\": \"object\",\r\n"
			"            \"additionalProperties\": { \"$ref\": \"#\" },\r\n"
			"            \"default\": {}\r\n"
			"        },\r\n"
			"        \"dependencies\": {\r\n"
			"            \"type\": \"object\",\r\n"
			"            \"additionalProperties\": {\r\n"
			"                \"anyOf\": [\r\n"
			"                    { \"$ref\": \"#\" },\r\n"
			"                    { \"$ref\": \"#/definitions/stringArray\" }\r\n"
			"                ]\r\n"
			"            }\r\n"
			"        },\r\n"
			"        \"propertyNames\": { \"$ref\": \"#\" },\r\n"
			"        \"const\": {},\r\n"
			"        \"enum\": {\r\n"
			"            \"type\": \"array\",\r\n"
			"            \"minItems\": 1,\r\n"
			"            \"uniqueItems\": true\r\n"
			"        },\r\n"
			"        \"type\": {\r\n"
			"            \"anyOf\": [\r\n"
			"                { \"$ref\": \"#/definitions/simpleTypes\" },\r\n"
			"                {\r\n"
			"                    \"type\": \"array\",\r\n"
			"                    \"items\": { \"$ref\": \"#/definitions/simpleTypes\" },\r\n"
			"                    \"minItems\": 1,\r\n"
			"                    \"uniqueItems\": true\r\n"
			"                }\r\n"
			"            ]\r\n"
			"        },\r\n"
			"        \"format\": { \"type\": \"string\" },\r\n"
			"        \"allOf\": { \"$ref\": \"#/definitions/schemaArray\" },\r\n"
			"        \"anyOf\": { \"$ref\": \"#/definitions/schemaArray\" },\r\n"
			"        \"oneOf\": { \"$ref\": \"#/definitions/schemaArray\" },\r\n"
			"        \"not\": { \"$ref\": \"#\" }\r\n"
			"    },\r\n"
			"    \"default\": {}\r\n"
			"}\r\n",
		.output =
			"{\"$schema\":\"http://json-schema.org/draft-06/schema#\","
			"\"$id\":\"http://json-schema.org/draft-06/schema#\","
			"\"title\":\"Core schema meta-schema\",\"definitions\":{"
			"\"schemaArray\":{\"type\":\"array\",\"minItems\":1,"
			"\"items\":{\"$ref\":\"#\"}},\"nonNegativeInteger\":{"
			"\"type\":\"integer\",\"minimum\":0},"
			"\"nonNegativeIntegerDefault0\":{\"allOf\":["
			"{\"$ref\":\"#/definitions/nonNegativeInteger\"},"
			"{\"default\":0}]},\"simpleTypes\":{\"enum\":["
			"\"array\",\"boolean\",\"integer\",\"null\","
			"\"number\",\"object\",\"string\"]},\"stringArray\":{"
			"\"type\":\"array\",\"items\":{\"type\":\"string\"},"
			"\"uniqueItems\":true,\"default\":[]}},"
			"\"type\":[\"object\",\"boolean\"],"
			"\"properties\":{\"$id\":{\"type\":\"string\","
			"\"format\":\"uri-reference\"},\"$schema\":{"
			"\"type\":\"string\",\"format\":\"uri\"},"
			"\"$ref\":{\"type\":\"string\",\"format\":\"uri-reference\""
			"},\"title\":{\"type\":\"string\"},\"description\":{"
			"\"type\":\"string\"},\"default\":{},\"multipleOf\":{"
			"\"type\":\"number\",\"exclusiveMinimum\":0},"
			"\"maximum\":{\"type\":\"number\"},\"exclusiveMaximum\":{"
			"\"type\":\"number\"},\"minimum\":{\"type\":\"number\""
			"},\"exclusiveMinimum\":{\"type\":\"number\"},"
			"\"maxLength\":{\"$ref\":\"#/definitions/nonNegativeInteger\"},"
			"\"minLength\":{\"$ref\":\"#/definitions/nonNegativeIntegerDefault0\"},"
			"\"pattern\":{\"type\":\"string\",\"format\":\"regex\""
			"},\"additionalItems\":{\"$ref\":\"#\"},\"items\":{"
			"\"anyOf\":[{\"$ref\":\"#\"},{\"$ref\":\"#/definitions/schemaArray\"}"
			"],\"default\":{}},"
			"\"maxItems\":{\"$ref\":\"#/definitions/nonNegativeInteger\"},"
			"\"minItems\":{\"$ref\":\"#/definitions/nonNegativeIntegerDefault0\"},"
			"\"uniqueItems\":{\"type\":\"boolean\",\"default\":false},"
			"\"contains\":{\"$ref\":\"#\"},"
			"\"maxProperties\":{\"$ref\":\"#/definitions/nonNegativeInteger\"},"
			"\"minProperties\":{\"$ref\":\"#/definitions/nonNegativeIntegerDefault0\"},"
			"\"required\":{\"$ref\":\"#/definitions/stringArray\"},"
			"\"additionalProperties\":{\"$ref\":\"#\"},\"definitions\":{"
			"\"type\":\"object\",\"additionalProperties\":{\"$ref\":\"#\"},"
			"\"default\":{}},\"properties\":{\"type\":\"object\","
			"\"additionalProperties\":{\"$ref\":\"#\"},\"default\":{}"
			"},\"patternProperties\":{\"type\":\"object\","
			"\"additionalProperties\":{\"$ref\":\"#\"},"
			"\"default\":{}},\"dependencies\":{\"type\":\"object\","
			"\"additionalProperties\":{\"anyOf\":[{\"$ref\":\"#\"},"
			"{\"$ref\":\"#/definitions/stringArray\"}"
			"]}},\"propertyNames\":{\"$ref\":\"#\"},\"const\":{},"
			"\"enum\":{\"type\":\"array\",\"minItems\":1,\"uniqueItems\":true"
			"},\"type\":{\"anyOf\":[{\"$ref\":\"#/definitions/simpleTypes\"},"
			"{\"type\":\"array\",\"items\":{\"$ref\":\"#/definitions/simpleTypes\"},"
			"\"minItems\":1,\"uniqueItems\":true}]},\"format\":{\"type\":\"string\"},"
			"\"allOf\":{\"$ref\":\"#/definitions/schemaArray\"},"
			"\"anyOf\":{\"$ref\":\"#/definitions/schemaArray\"},"
			"\"oneOf\":{\"$ref\":\"#/definitions/schemaArray\"},"
			"\"not\":{\"$ref\":\"#\"}},\"default\":{}}"
	},
	/* escape sequences */
	{
		.input = "\"\\u0020\"",
		.output = "\" \"",
	},{
		.input = "\"\\u0020\\u0020\"",
		.output = "\"  \"",
	},{
		.input = "\"\\\"\"",
		.output = "\"\\\"\"",
	},{
		.input = "\"\\\\\"",
		.output = "\"\\\\\"",
	},{
		.input = "\"\\/\"",
		.output = "\"/\"",
	},{
		.input = "\"\\b\"",
		.output = "\"\\b\"",
	},{
		.input = "\"\\f\"",
		.output = "\"\\f\"",
	},{
		.input = "\"\\n\"",
		.output = "\"\\n\"",
	},{
		.input = "\"\\r\"",
		.output = "\"\\r\"",
	},{
		.input = "\"\\t\"",
		.output = "\"\\t\"",
	},{
		.input = "\"\\u0020\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
		.output = "\" \\\"\\\\/\\b\\f\\n\\r\\t\"",
	},{
		.input = "\"\\u00a2\"",
		.output = "\"\xC2\xA2\""
	},{
		.input = "\"\\u20AC\"",
		.output = "\"\xE2\x82\xAC\""
	},{
		.input = "\"\\uD808\\uDC00\"",
		.output = "\"\xF0\x92\x80\x80\""
	},{
		.input = "\"\\u00a2\\u20AC\\uD808\\uDC00\"",
		.output = "\"\xC2\xA2\xE2\x82\xAC\xF0\x92\x80\x80\""
	},{
		.input = "\"\\uD81A\\uDCD8\"",
		.output = "\"\xF0\x96\xA3\x98\""
	},{
		.input = "\"\\uD836\\uDD49\"",
		.output = "\"\xF0\x9D\xA5\x89\"",
	},{
		.input = "\"\xF0\x92\x80\x80\"",
		.output = "\"\xF0\x92\x80\x80\""
	},{
		.input = "\"\xF0\x96\xA3\x98\"",
		.output = "\"\xF0\x96\xA3\x98\""
	},{
		.input = "\"\xF0\x9D\xA5\x89\"",
		.output = "\"\xF0\x9D\xA5\x89\"",
	},{
		.input = "\"\\\\xFF\\\\xFF\\\\xFF\"",
		.output = "\"\\\\xFF\\\\xFF\\\\xFF\"",
	},{
		.input = "\"\xe2\x80\xa8\xe2\x80\xa9\"",
		.output = "\"\\u2028\\u2029\"",
	}
};

static const unsigned tests_count = N_ELEMENTS(tests);

/*
 * Low-level I/O
 */

struct test_io_context;

enum test_io_state {
	TEST_STATE_NONE = 0,
	TEST_STATE_OBJECT_OPEN,
	TEST_STATE_ARRAY_OPEN,
	TEST_STATE_OBJECT_MEMBER,
	TEST_STATE_VALUE,
	TEST_STATE_OBJECT_CLOSE,
	TEST_STATE_ARRAY_CLOSE,
};

struct test_io_processor {
	struct test_io_context *tctx;
	const char *name;

	string_t *membuf, *strbuf;
	struct ostream *output;
	struct istream *input;
	struct io *io;

	enum test_io_state state;

	enum json_type type;
	struct json_value value;
	struct json_data data;

	struct json_generator *generator;
	struct json_parser *parser;

	unsigned int pos;
};

struct test_io_context {
	const struct json_io_test *test;
	unsigned int scenario;

	struct iostream_pump *pump_in, *pump_out;
};

static void
test_copy_value(struct test_io_processor *tproc, enum json_type type,
		const struct json_value *value)
{
	tproc->type = type;
	tproc->value = *value;
	switch (value->content_type) {
	case JSON_CONTENT_TYPE_STRING:
		str_truncate(tproc->strbuf, 0);
		str_append(tproc->strbuf, value->content.str);
		tproc->value.content.str = str_c(tproc->strbuf);
		break;
	case JSON_CONTENT_TYPE_DATA:
		tproc->data = *value->content.data;
		tproc->value.content.data = &tproc->data;
		str_truncate(tproc->strbuf, 0);
		str_append_data(tproc->strbuf, tproc->data.data,
				tproc->data.size);
		tproc->data.data = str_data(tproc->strbuf);
		break;
	default:
		break;
	}
}

static void
test_parse_list_open(void *context, void *parent_context ATTR_UNUSED,
		     const char *name, bool object,
		     void **list_context_r ATTR_UNUSED)
{
	struct test_io_processor *tproc = context;
	int ret;

	if (object) {
		tproc->state = TEST_STATE_OBJECT_OPEN;
	} else {
		tproc->state = TEST_STATE_ARRAY_OPEN;
	}

	if (name != NULL) {
		ret = json_generate_object_member(tproc->generator, name);
		if (ret <= 0) {
			str_truncate(tproc->membuf, 0);
			str_append(tproc->membuf, name);
			json_parser_interrupt(tproc->parser);
			return;
		}
	}

	tproc->state = TEST_STATE_NONE;
	if (object)
		json_generate_object_open(tproc->generator);
	else
		json_generate_array_open(tproc->generator);
}

static void
test_parse_list_close(void *context, void *list_context ATTR_UNUSED,
		      bool object)
{
	struct test_io_processor *tproc = context;
	int ret;

	if (object) {
		tproc->state = TEST_STATE_OBJECT_CLOSE;
		ret = json_generate_object_close(tproc->generator);
	} else {
		tproc->state = TEST_STATE_ARRAY_CLOSE;
		ret = json_generate_array_close(tproc->generator);
	}
	if (ret <= 0) {
		json_parser_interrupt(tproc->parser);
		return;
	}

	tproc->state = TEST_STATE_NONE;
}

static void
test_parse_value(void *context, void *parent_context ATTR_UNUSED,
		 const char *name, enum json_type type,
		 const struct json_value *value)
{
	struct test_io_processor *tproc = context;
	int ret;

	tproc->state = TEST_STATE_OBJECT_MEMBER;

	if (name != NULL) {
		ret = json_generate_object_member(tproc->generator, name);
		if (ret <= 0) {
			str_truncate(tproc->membuf, 0);
			str_append(tproc->membuf, name);
			json_parser_interrupt(tproc->parser);
			test_copy_value(tproc, type, value);
			return;
		}
	}

	tproc->state = TEST_STATE_VALUE;

	ret = json_generate_value(tproc->generator, type, value);
	if (ret <= 0) {
		if (ret == 0)
			test_copy_value(tproc, type, value);
		json_parser_interrupt(tproc->parser);
		return;
	}

	tproc->state = TEST_STATE_NONE;
}

static int test_write(struct test_io_processor *tproc)
{
	int ret;

	switch (tproc->state) {
	case TEST_STATE_NONE:
		break;
	case TEST_STATE_OBJECT_OPEN:
		ret = json_generate_object_member(tproc->generator,
						  str_c(tproc->membuf));
		if (ret <= 0)
			return ret;
		tproc->state = TEST_STATE_VALUE;
		json_generate_object_open(tproc->generator);
		break;
	case TEST_STATE_ARRAY_OPEN:
		ret = json_generate_object_member(tproc->generator,
						  str_c(tproc->membuf));
		if (ret <= 0)
			return ret;
		tproc->state = TEST_STATE_VALUE;
		json_generate_array_open(tproc->generator);
		break;
	case TEST_STATE_OBJECT_MEMBER:
		ret = json_generate_object_member(tproc->generator,
						  str_c(tproc->membuf));
		if (ret <= 0)
			return ret;
		tproc->state = TEST_STATE_VALUE;
		/* fall through */
	case TEST_STATE_VALUE:
		ret = json_generate_value(tproc->generator,
					  tproc->type, &tproc->value);
		if (ret <= 0)
			return ret;
		break;
	case TEST_STATE_OBJECT_CLOSE:
		ret = json_generate_object_close(tproc->generator);
		if (ret <= 0)
			return ret;
		break;
	case TEST_STATE_ARRAY_CLOSE:
		ret = json_generate_array_close(tproc->generator);
		if (ret <= 0)
			return ret;
		break;
	}

	tproc->state = TEST_STATE_NONE;
	return 1;
}

struct json_parser_callbacks parser_callbacks = {
	.parse_list_open = test_parse_list_open,
	.parse_list_close = test_parse_list_close,

	.parse_value = test_parse_value
};

static void
test_io_processor_init(struct test_io_processor *tproc,
		       const struct json_io_test *test,
		       struct istream *input, struct ostream *output)
{
	i_zero(tproc);
	tproc->membuf = str_new(default_pool, 256);;
	tproc->strbuf = str_new(default_pool, 256);

	tproc->output = output;
	o_stream_set_no_error_handling(tproc->output, TRUE);
	tproc->input = input;

	tproc->parser = json_parser_init(
		tproc->input, &test->limits, test->flags,
		&parser_callbacks, tproc);
	tproc->generator = json_generator_init(tproc->output, 0);
}

static void test_io_processor_deinit(struct test_io_processor *tproc)
{
	json_generator_deinit(&tproc->generator);
	json_parser_deinit(&tproc->parser);

	buffer_free(&tproc->strbuf);
	buffer_free(&tproc->membuf);
}

static void test_json_io(void)
{
	static const unsigned int margins[] = { 0, 1, 2, 10, 50 };
	string_t *outbuf;
	unsigned int i, j;

	outbuf = str_new(default_pool, 256);

	for (i = 0; i < tests_count; i++) T_BEGIN {
		const struct json_io_test *test;
		struct test_io_processor tproc;
		const char *text, *text_out;
		unsigned int pos, margin, text_len;

		test = &tests[i];
		text = test->input;
		text_out = test->output;
		if (text_out == NULL)
			text_out = test->input;
		text_len = strlen(text);

		test_begin(t_strdup_printf("json io [%d]", i));

		for (j = 0; j < N_ELEMENTS(margins); j++) {
			struct istream *input;
			struct ostream *output;
			const char *error = NULL;
			int pret = 0, wret = 0;

			margin = margins[j];

			buffer_set_used_size(outbuf, 0);

			input = test_istream_create_data(text, text_len);
			output = o_stream_create_buffer(outbuf);
			test_io_processor_init(&tproc, test, input, output);

			o_stream_set_max_buffer_size(output, 0);
			pret = 0; wret = 1;
			for (pos = 0;
				pos <= (text_len+margin) &&
					(pret == 0 || wret == 0);
				pos++) {
				test_istream_set_size(input, pos);
				o_stream_set_max_buffer_size(output,
					(pos > margin ? pos - margin : 0));
				if (wret > 0 && pret == 0) {
					pret = json_parse_more(tproc.parser,
							       &error);
					if (pret < 0)
						break;
				}
				wret = test_write(&tproc);
				if (wret == 0)
					continue;
				if (wret < 0)
					break;
			}

			if (pret == 0)
				pret = json_parse_more(tproc.parser, &error);

			o_stream_set_max_buffer_size(output, SIZE_MAX);
			wret = json_generator_flush(tproc.generator);

			test_out_reason_quiet(
				t_strdup_printf("parse success "
						"(trickle, margin=%u)", margin),
				pret > 0, error);
			test_out_quiet(
				t_strdup_printf("write success "
						"(trickle, margin=%u)", margin),
				wret > 0);
			test_out_quiet(
				t_strdup_printf("io match (trickle, margin=%u)",
						margin),
				strcmp(text_out, str_c(outbuf)) == 0);
			if (debug) {
				i_debug("OUT: >%s<", text_out);
				i_debug("OUT: >%s<", str_c(outbuf));
			}

			test_io_processor_deinit(&tproc);
			i_stream_destroy(&input);
			o_stream_destroy(&output);
		}

		test_end();

	} T_END;

	buffer_free(&outbuf);
}

static void test_json_async_io_input_callback(struct test_io_processor *tproc)
{
	const char *error;
	int ret;

	ret = json_parse_more(tproc->parser, &error);
	if (ret == 0) {
		ret = test_write(tproc);
		if (ret == 0) {
			o_stream_set_flush_pending(tproc->output, TRUE);
			io_remove(&tproc->io);
			return;
		}
		if (ret < 0) {
			test_assert(FALSE);
			io_loop_stop(current_ioloop);
		}
		return;
	}

	test_out_reason_quiet(
		t_strdup_printf("%u: %s: parse success (async)",
				tproc->tctx->scenario, tproc->name),
		ret > 0, error);
	if (ret < 0) {
		io_loop_stop(current_ioloop);
	} else {
		ret = test_write(tproc);
		if (ret > 0)
			ret = json_generator_flush(tproc->generator);
		if (ret == 0) {
			o_stream_set_flush_pending(tproc->output, TRUE);
			io_remove(&tproc->io);
			return;
		}
		test_out_quiet(t_strdup_printf("%u: %s: write success (async)",
					       tproc->tctx->scenario,
					       tproc->name), ret > 0);
		if (ret < 0) {
			io_loop_stop(current_ioloop);
			return;
		}

		io_remove(&tproc->io);
		o_stream_close(tproc->output);
	}
}

static int test_json_async_io_flush_callback(struct test_io_processor *tproc)
{
	int ret;

	ret = json_generator_flush(tproc->generator);
	if (ret == 0)
		return ret;
	if (ret < 0) {
		test_assert(FALSE);
		io_loop_stop(current_ioloop);
		return -1;
	}

	ret = test_write(tproc);
	if (ret == 0)
		return 0;
	if (ret < 0) {
		test_assert(FALSE);
		io_loop_stop(current_ioloop);
		return -1;
	}

	if (tproc->io == NULL) {
		tproc->io = io_add_istream(
			tproc->input, test_json_async_io_input_callback, tproc);
		i_stream_set_input_pending(tproc->input, TRUE);
	}
	return 1;
}

static void
test_json_async_io_pump_in_callback(enum iostream_pump_status status,
				    struct test_io_context *tctx)
{
	if (status != IOSTREAM_PUMP_STATUS_INPUT_EOF) {
		test_assert(FALSE);
		io_loop_stop(current_ioloop);
		return;
	}

	struct ostream *output = iostream_pump_get_output(tctx->pump_in);

	o_stream_close(output);
	iostream_pump_destroy(&tctx->pump_in);
}

static void
test_json_async_io_pump_out_callback(enum iostream_pump_status status,
				     struct test_io_context *tctx)
{
	if (status != IOSTREAM_PUMP_STATUS_INPUT_EOF)
		test_assert(FALSE);

	io_loop_stop(current_ioloop);
	iostream_pump_destroy(&tctx->pump_out);
}

static void
test_json_async_io_run(const struct json_io_test *test, unsigned int scenario)
{
	struct test_io_context tctx;
	string_t *outbuf;
	struct test_io_processor tproc1, tproc2;
	struct ioloop *ioloop;
	int fd_pipe1[2], fd_pipe2[2], fd_pipe3[2];
	const char *text, *text_out;
	unsigned int text_len;
	struct istream *input, *pipe1_input, *pipe2_input, *pipe3_input;
	struct ostream *output, *pipe1_output, *pipe2_output, *pipe3_output;

	i_zero(&tctx);
	tctx.test = test;
	tctx.scenario = scenario;

	text = test->input;
	text_out = test->output;
	if (text_out == NULL)
		text_out = test->input;
	text_len = strlen(text);

	outbuf = str_new(default_pool, 256);

	if (pipe(fd_pipe1) < 0)
		i_fatal("pipe() failed: %m");
	if (pipe(fd_pipe2) < 0)
		i_fatal("pipe() failed: %m");
	if (pipe(fd_pipe3) < 0)
		i_fatal("pipe() failed: %m");
	fd_set_nonblock(fd_pipe1[0], TRUE);
	fd_set_nonblock(fd_pipe1[1], TRUE);
	fd_set_nonblock(fd_pipe2[0], TRUE);
	fd_set_nonblock(fd_pipe2[1], TRUE);
	fd_set_nonblock(fd_pipe3[0], TRUE);
	fd_set_nonblock(fd_pipe3[1], TRUE);

	ioloop = io_loop_create();

	input = i_stream_create_from_data(text, text_len);
	output = o_stream_create_buffer(outbuf);

	switch (scenario) {
	case 0: case 2:
		pipe1_input = i_stream_create_fd_autoclose(&fd_pipe1[0], 16);
		pipe2_input = i_stream_create_fd_autoclose(&fd_pipe2[0], 32);
		pipe3_input = i_stream_create_fd_autoclose(&fd_pipe3[0], 64);
		break;
	case 1: case 3:
		pipe1_input = i_stream_create_fd_autoclose(&fd_pipe1[0], 128);
		pipe2_input = i_stream_create_fd_autoclose(&fd_pipe2[0], 64);
		pipe3_input = i_stream_create_fd_autoclose(&fd_pipe3[0], 32);
		break;
	default:
		i_unreached();
	}

	switch (scenario) {
	case 0: case 1:
		pipe1_output = o_stream_create_fd_autoclose(&fd_pipe1[1], 32);
		pipe2_output = o_stream_create_fd_autoclose(&fd_pipe2[1], 64);
		pipe3_output = o_stream_create_fd_autoclose(&fd_pipe3[1], 128);
		break;
	case 2: case 3:
		pipe1_output = o_stream_create_fd_autoclose(&fd_pipe1[1], 64);
		pipe2_output = o_stream_create_fd_autoclose(&fd_pipe2[1], 32);
		pipe3_output = o_stream_create_fd_autoclose(&fd_pipe3[1], 16);
		break;
	default:
		i_unreached();
	}

	tctx.pump_in = iostream_pump_create(input, pipe1_output);
	tctx.pump_out = iostream_pump_create(pipe3_input, output);

	iostream_pump_set_completion_callback(
		tctx.pump_in, test_json_async_io_pump_in_callback, &tctx);
	iostream_pump_set_completion_callback(
		tctx.pump_out, test_json_async_io_pump_out_callback, &tctx);

	/* Processor 1 */
	test_io_processor_init(&tproc1, test, pipe1_input, pipe2_output);
	tproc1.tctx = &tctx;
	tproc1.name = "proc_a";
	o_stream_uncork(tproc1.output);

	o_stream_set_flush_callback(tproc1.output,
				    test_json_async_io_flush_callback, &tproc1);
	tproc1.io = io_add_istream(tproc1.input,
				  test_json_async_io_input_callback, &tproc1);

	/* Processor 2 */
	test_io_processor_init(&tproc2, test, pipe2_input, pipe3_output);
	tproc2.tctx = &tctx;
	tproc2.name = "proc_b";
	o_stream_uncork(tproc2.output);

	o_stream_set_flush_callback(tproc2.output,
				    test_json_async_io_flush_callback, &tproc2);
	tproc2.io = io_add_istream(tproc2.input,
				  test_json_async_io_input_callback, &tproc2);

	struct timeout *to = timeout_add(5000, io_loop_stop, ioloop);

	iostream_pump_start(tctx.pump_in);
	iostream_pump_start(tctx.pump_out);

	io_loop_run(ioloop);

	timeout_remove(&to);

	test_io_processor_deinit(&tproc1);
	test_io_processor_deinit(&tproc2);

	iostream_pump_destroy(&tctx.pump_in);
	iostream_pump_destroy(&tctx.pump_out);

	i_stream_destroy(&input);
	i_stream_destroy(&pipe1_input);
	i_stream_destroy(&pipe2_input);
	i_stream_destroy(&pipe3_input);

	o_stream_destroy(&output);
	o_stream_destroy(&pipe1_output);
	o_stream_destroy(&pipe2_output);
	o_stream_destroy(&pipe3_output);

	io_loop_destroy(&ioloop);

	test_out_quiet(t_strdup_printf("%u: io match (async)", scenario),
		       strcmp(text_out, str_c(outbuf)) == 0);

	buffer_free(&outbuf);
}

static void test_json_io_async(void)
{
	unsigned int i, sc;

	for (i = 0; i < tests_count; i++) T_BEGIN {
		test_begin(t_strdup_printf("json io async [%d]", i));

		for (sc = 0; sc < 4; sc++)
			test_json_async_io_run(&tests[i], sc);

		test_end();
	} T_END;
}

int main(int argc, char *argv[])
{
	int ret, c;

	random_init();

	static void (*test_functions[])(void) = {
		test_json_io,
		test_json_io_async,
		NULL
	};

	while ((c = getopt(argc, argv, "D")) > 0) {
		switch (c) {
		case 'D':
			debug = TRUE;
			break;
		default:
			i_fatal("Usage: %s [-D]", argv[0]);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0)
		i_fatal("Usage: %s [-D]", argv[0]);

	ret = test_run(test_functions);

	random_deinit();

	return ret;
}
