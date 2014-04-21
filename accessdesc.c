#include "lib.h"
#include "ncch.h"
#include "exheader.h"
#include "accessdesc.h"

#include "polarssl/base64.h"

#include "desc_presets.h"
#ifndef PUBLIC_BUILD
#include "desc_dev_sigdata.h"
#include "desc_prod_sigdata.h"
#endif

const int RSF_RSA_DATA_LEN = 344;
const int RSF_DESC_DATA_LEN = 684;


int accessdesc_SignWithKey(exheader_settings *exhdrset, ncch_settings *ncchset);
int accessdesc_GetSignFromRsf(exheader_settings *exhdrset, ncch_settings *ncchset);
int accessdesc_GetSignFromPreset(exheader_settings *exhdrset, ncch_settings *ncchset);
void accessdesc_GetPresetData(u8 **desc, u8 **accessDesc, u8 **depList, ncch_settings *ncchset);
#ifndef PUBLIC_BUILD
void accessdesc_GetPresetSigData(u8 **accessDescSig, u8 **cxiPubk, u8 **cxiPvtk, ncch_settings *ncchset);
#endif

bool IsValidB64Char(char chr);
u32 b64_strlen(char *str);
void b64_strcpy(char *dst, char *src);

int set_AccessDesc(exheader_settings *exhdrset, ncch_settings *ncchset)
{
	if(ncchset->keys->accessDescSign.presetType == not_preset){
		if(ncchset->rsfSet->CommonHeaderKey.Found) // Keydata exists in RSF
			return accessdesc_GetSignFromRsf(exhdrset,ncchset);
		else if(!ncchset->keys->rsa.requiresPresignedDesc) // Else if The AccessDesc can be signed with key
			return accessdesc_SignWithKey(exhdrset,ncchset);
		else{ // No way the access desc signature can be 'obtained'
			fprintf(stderr,"[EXHEADER ERROR] Current keyset cannot sign AccessDesc, please appropriatly setup RSF, or specify a preset with -accessdesc\n");
			return CANNOT_SIGN_ACCESSDESC;
		}
	}
	return accessdesc_GetSignFromPreset(exhdrset,ncchset);
}

int accessdesc_SignWithKey(exheader_settings *exhdrset, ncch_settings *ncchset)
{
	/* Set RSA Keys */
	memcpy(exhdrset->keys->rsa.cxiHdrPvt,exhdrset->keys->rsa.cciCfaPvt,0x100);
	memcpy(exhdrset->keys->rsa.cxiHdrPub,exhdrset->keys->rsa.cciCfaPub,0x100);
	memcpy(&exhdrset->exHdr->accessDescriptor.ncchRsaPubKey,exhdrset->keys->rsa.cxiHdrPub,0x100);
	/* Copy Data From ExHeader */
	memcpy(&exhdrset->exHdr->accessDescriptor.arm11SystemLocalCapabilities,&exhdrset->exHdr->arm11SystemLocalCapabilities,sizeof(exhdr_ARM11SystemLocalCapabilities));
	u8 *flag = &exhdrset->exHdr->accessDescriptor.arm11SystemLocalCapabilities.flag;
	u8 SystemMode = (*flag>>4)&0xF;
	u8 AffinityMask = (*flag>>2)&0x3;
	u8 IdealProcessor = 1<<((*flag>>0)&0x3);
	*flag = (u8)(SystemMode << 4 | AffinityMask << 2 | IdealProcessor);
	exhdrset->exHdr->accessDescriptor.arm11SystemLocalCapabilities.priority /= 2;
	
	memcpy(&exhdrset->exHdr->accessDescriptor.arm11KernelCapabilities,&exhdrset->exHdr->arm11KernelCapabilities,sizeof(exhdr_ARM11KernelCapabilities));
	memcpy(&exhdrset->exHdr->accessDescriptor.arm9AccessControlInfo,&exhdrset->exHdr->arm9AccessControlInfo,sizeof(exhdr_ARM9AccessControlInfo));
	/* Sign AccessDesc */
	return SignAccessDesc(exhdrset->exHdr,exhdrset->keys);
}

int accessdesc_GetSignFromRsf(exheader_settings *exhdrset, ncch_settings *ncchset)
{
	/* Yaml Option Sanity Checks */
	if(!exhdrset->rsf->CommonHeaderKey.Found){
		fprintf(stderr,"[EXHEADER ERROR] RSF Section \"CommonHeaderKey\" not found\n");
		return COMMON_HEADER_KEY_NOT_FOUND;
	}

	if(!exhdrset->rsf->CommonHeaderKey.D){
		ErrorParamNotFound("CommonHeaderKey/D");
		return COMMON_HEADER_KEY_NOT_FOUND;
	}
	if(b64_strlen(exhdrset->rsf->CommonHeaderKey.D) != RSF_RSA_DATA_LEN){
		fprintf(stderr,"[EXHEADER ERROR] \"CommonHeaderKey/D\" has invalid length (%d)\n",b64_strlen(exhdrset->rsf->CommonHeaderKey.D));
		return COMMON_HEADER_KEY_NOT_FOUND;
	}

	if(!exhdrset->rsf->CommonHeaderKey.Modulus){
		ErrorParamNotFound("CommonHeaderKey/Modulus");
		return COMMON_HEADER_KEY_NOT_FOUND;
	}
	if(b64_strlen(exhdrset->rsf->CommonHeaderKey.Modulus) != RSF_RSA_DATA_LEN){
		fprintf(stderr,"[EXHEADER ERROR] \"CommonHeaderKey/Modulus\" has invalid length (%d)\n",b64_strlen(exhdrset->rsf->CommonHeaderKey.Modulus));
		return COMMON_HEADER_KEY_NOT_FOUND;
	}

	if(!exhdrset->rsf->CommonHeaderKey.AccCtlDescSign){
		ErrorParamNotFound("CommonHeaderKey/Signature");
		return COMMON_HEADER_KEY_NOT_FOUND;
	}
	if(b64_strlen(exhdrset->rsf->CommonHeaderKey.AccCtlDescSign) != RSF_RSA_DATA_LEN){
		fprintf(stderr,"[EXHEADER ERROR] \"CommonHeaderKey/Signature\" has invalid length (%d)\n",b64_strlen(exhdrset->rsf->CommonHeaderKey.AccCtlDescSign));
		return COMMON_HEADER_KEY_NOT_FOUND;
	}

	if(!exhdrset->rsf->CommonHeaderKey.AccCtlDescBin){
		ErrorParamNotFound("CommonHeaderKey/Descriptor");
		return COMMON_HEADER_KEY_NOT_FOUND;
	}
	if(b64_strlen(exhdrset->rsf->CommonHeaderKey.AccCtlDescBin) != RSF_DESC_DATA_LEN){
		fprintf(stderr,"[EXHEADER ERROR] \"CommonHeaderKey/Descriptor\" has invalid length (%d)\n",b64_strlen(exhdrset->rsf->CommonHeaderKey.AccCtlDescBin));
		return COMMON_HEADER_KEY_NOT_FOUND;
	}

	/* Set RSA Keys */
	int result = 0;
	u32 out;

	out = 0x100;
	result = base64_decode(exhdrset->keys->rsa.cxiHdrPub,&out,(const u8*)exhdrset->rsf->CommonHeaderKey.Modulus,strlen(exhdrset->rsf->CommonHeaderKey.Modulus));
	if(out != 0x100)
		result = POLARSSL_ERR_BASE64_BUFFER_TOO_SMALL;
	if(result) goto finish;

	out = 0x100;
	result = base64_decode(exhdrset->keys->rsa.cxiHdrPvt,&out,(const u8*)exhdrset->rsf->CommonHeaderKey.D,strlen(exhdrset->rsf->CommonHeaderKey.D));
	if(out != 0x100)
		result = POLARSSL_ERR_BASE64_BUFFER_TOO_SMALL;
	if(result) goto finish;

	/* Set AccessDesc */
	out = 0x100;
	result = base64_decode(exhdrset->exHdr->accessDescriptor.signature,&out,(const u8*)exhdrset->rsf->CommonHeaderKey.AccCtlDescSign, strlen( exhdrset->rsf->CommonHeaderKey.AccCtlDescSign));
	if(out != 0x100)
		result = POLARSSL_ERR_BASE64_BUFFER_TOO_SMALL;
	if(result) goto finish;
	memcpy(exhdrset->exHdr->accessDescriptor.ncchRsaPubKey,exhdrset->keys->rsa.cxiHdrPub,0x100);

	out = 0x200;
	result = base64_decode((u8*)&exhdrset->exHdr->accessDescriptor.arm11SystemLocalCapabilities,&out,(const u8*)exhdrset->rsf->CommonHeaderKey.AccCtlDescBin,strlen(exhdrset->rsf->CommonHeaderKey.AccCtlDescBin));
	if(out != 0x200)
		result = POLARSSL_ERR_BASE64_BUFFER_TOO_SMALL;
	if(result) goto finish;
finish:
	return result;	
}

int accessdesc_GetSignFromPreset(exheader_settings *exhdrset, ncch_settings *ncchset)
{
	u8 *desc = NULL;
	u8 *accessDesc = NULL;
	u8 *depList = NULL;

	u8 *accessDescSig = NULL;
	u8 *cxiPubk = NULL;
	u8 *cxiPvtk = NULL;

	accessdesc_GetPresetData(&desc,&accessDesc,&depList,ncchset);
#ifndef PUBLIC_BUILD
	accessdesc_GetPresetSigData(&accessDescSig,&cxiPubk,&cxiPvtk,ncchset);
#endif

	// Error Checking
	if(!accessDesc || !depList){
		fprintf(stderr,"[EXHEADER ERROR] AccessDesc preset is unavailable, please configure RSF file\n");
		return CANNOT_SIGN_ACCESSDESC;
	}

	if((!cxiPubk || !cxiPvtk || !accessDescSig) && ncchset->keys->rsa.requiresPresignedDesc){
		fprintf(stderr,"[EXHEADER ERROR] This AccessDesc preset needs to be signed, the current keyset is incapable of doing so. Please configure RSF file with the appropriate signature data.\n");
		return CANNOT_SIGN_ACCESSDESC;
	}
	
	// Setting data in Exheader
	// Dependency List
	memcpy(exhdrset->exHdr->dependencyList,depList,0x180);

	// ARM11 Local Capabilities
	exhdr_ARM11SystemLocalCapabilities *arm11local = (exhdr_ARM11SystemLocalCapabilities*)(accessDesc);
	// Backing Up Non Preset Details
	u8 ProgramID[8];
	exhdr_StorageInfo StorageInfoBackup;
	memcpy(ProgramID,exhdrset->exHdr->arm11SystemLocalCapabilities.programId,8);
	memcpy(&StorageInfoBackup,&exhdrset->exHdr->arm11SystemLocalCapabilities.storageInfo,sizeof(exhdr_StorageInfo));
	
	// Setting Preset Data
	memcpy(&exhdrset->exHdr->arm11SystemLocalCapabilities,arm11local,sizeof(exhdr_ARM11SystemLocalCapabilities));

	// Restoring Non Preset Data
	memcpy(exhdrset->exHdr->arm11SystemLocalCapabilities.programId,ProgramID,8);
	memcpy(&exhdrset->exHdr->arm11SystemLocalCapabilities.storageInfo,&StorageInfoBackup,sizeof(exhdr_StorageInfo));

	// Adjusting flags to prevent errors
	u8 *flag = &exhdrset->exHdr->arm11SystemLocalCapabilities.flag;
	u8 SystemMode = (*flag>>4)&0xF;
	u8 AffinityMask = (*flag>>2)&0x3;
	u8 IdealProcessor = ((*flag>>0)&0x3)>>1;
	*flag = (u8)(SystemMode << 4 | AffinityMask << 2 | IdealProcessor);
	exhdrset->exHdr->arm11SystemLocalCapabilities.priority *= 2;

	// ARM11 Kernel Capabilities
	exhdr_ARM11KernelCapabilities *arm11kernel = (exhdr_ARM11KernelCapabilities*)(accessDesc+sizeof(exhdr_ARM11SystemLocalCapabilities));
	memcpy(&exhdrset->exHdr->arm11KernelCapabilities,arm11kernel,(sizeof(exhdr_ARM11KernelCapabilities)));

	// ARM9 Access Control
	exhdr_ARM9AccessControlInfo *arm9 = (exhdr_ARM9AccessControlInfo*)(accessDesc+sizeof(exhdr_ARM11SystemLocalCapabilities)+sizeof(exhdr_ARM11KernelCapabilities));
	memcpy(&exhdrset->exHdr->arm9AccessControlInfo,arm9,(sizeof(exhdr_ARM9AccessControlInfo)));

	// Setting AccessDesc Area
	// Signing normally if possible
	if(!ncchset->keys->rsa.requiresPresignedDesc) 
		return accessdesc_SignWithKey(exhdrset,ncchset);

	// Otherwise set static data & ncch hdr sig info
	memcpy(exhdrset->keys->rsa.cxiHdrPub,cxiPubk,0x100);
	memcpy(exhdrset->keys->rsa.cxiHdrPvt,cxiPvtk,0x100);
	memcpy(&exhdrset->exHdr->accessDescriptor.signature,accessDescSig,0x100);
	memcpy(&exhdrset->exHdr->accessDescriptor.ncchRsaPubKey,cxiPubk,0x100);
	memcpy(&exhdrset->exHdr->accessDescriptor.arm11SystemLocalCapabilities,accessDesc,0x200);

	return 0;
}

void accessdesc_GetPresetData(u8 **desc, u8 **accessDesc, u8 **depList, ncch_settings *ncchset)
{
	if(ncchset->keys->accessDescSign.presetType == app){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 1:
				*desc = (u8*)app_1_desc_data;
				*accessDesc = (u8*)app_1_acex_data;
				*depList = (u8*)sdk1_dep_list;
				break;
			case 2:
				*desc = (u8*)app_2_desc_data;
				*accessDesc = (u8*)app_2_acex_data;
				*depList = (u8*)sdk2_dep_list;
				break;
			case 3:
				*desc = (u8*)app_3_desc_data;
				*accessDesc = (u8*)app_3_acex_data;
				*depList = (u8*)sdk3_dep_list;
				break;
			case 4:
				*desc = (u8*)app_4_desc_data;
				*accessDesc = (u8*)app_4_acex_data;
				*depList = (u8*)sdk4_dep_list;
				break;
			case 5:
				*desc = (u8*)app_5_desc_data;
				*accessDesc = (u8*)app_5_acex_data;
				*depList = (u8*)sdk5_dep_list;
				break;
			case 7:
				*desc = (u8*)app_7_desc_data;
				*accessDesc = (u8*)app_7_acex_data;
				*depList = (u8*)sdk7_dep_list;
				break;
			
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == ec_app){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 4:
				*desc = (u8*)ecapp_4_desc_data;
				*accessDesc = (u8*)ecapp_4_acex_data;
				*depList = (u8*)sdk4_dep_list;
				break;
			case 5:
				*desc = (u8*)ecapp_5_desc_data;
				*accessDesc = (u8*)ecapp_5_acex_data;
				*depList = (u8*)sdk5_dep_list;
				break;
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == dlp){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 1:
				*desc = (u8*)dlp_1_desc_data;
				*accessDesc = (u8*)dlp_1_acex_data;
				*depList = (u8*)sdk1_dep_list;
				break;
			case 2:
				*desc = (u8*)dlp_2_desc_data;
				*accessDesc = (u8*)dlp_2_acex_data;
				*depList = (u8*)sdk2_dep_list;
				break;
			case 4:
				*desc = (u8*)dlp_4_desc_data;
				*accessDesc = (u8*)dlp_4_acex_data;
				*depList = (u8*)sdk4_dep_list;
				break;
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == demo){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 2:
				*desc = (u8*)demo_2_desc_data;
				*accessDesc = (u8*)demo_2_acex_data;
				*depList = (u8*)sdk2_dep_list;
				break;
			case 4:
				*desc = (u8*)demo_4_desc_data;
				*accessDesc = (u8*)demo_4_acex_data;
				*depList = (u8*)sdk4_dep_list;
				break;
		}
	}
}

#ifndef PUBLIC_BUILD
void accessdesc_GetPresetSigData(u8 **accessDescSig, u8 **cxiPubk, u8 **cxiPvtk, ncch_settings *ncchset)
{
	if(ncchset->keys->accessDescSign.presetType == app){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 1:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)app_1_dev_acexsig;
					*cxiPubk = (u8*)app_1_dev_hdrpub;
					*cxiPvtk = (u8*)app_1_dev_hdrpvt;
				}
				break;
			case 2:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)app_2_dev_acexsig;
					*cxiPubk = (u8*)app_2_dev_hdrpub;
					*cxiPvtk = (u8*)app_2_dev_hdrpvt;
				}
				if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)app_2_prod_acexsig;
					*cxiPubk = (u8*)app_2_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			case 3:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)app_3_dev_acexsig;
					*cxiPubk = (u8*)app_3_dev_hdrpub;
					*cxiPvtk = NULL;
				}
				else if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)app_3_prod_acexsig;
					*cxiPubk = (u8*)app_3_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			case 4:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)app_4_dev_acexsig;
					*cxiPubk = (u8*)app_4_dev_hdrpub;
					*cxiPvtk = (u8*)app_4_dev_hdrpvt;
				}
				else if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)app_4_prod_acexsig;
					*cxiPubk = (u8*)app_4_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			case 5:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)app_5_dev_acexsig;
					*cxiPubk = (u8*)app_5_dev_hdrpub;
					*cxiPvtk = NULL;
				}
				else if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)app_5_prod_acexsig;
					*cxiPubk = (u8*)app_5_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			case 7:
				if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)app_7_prod_acexsig;
					*cxiPubk = (u8*)app_7_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == ec_app){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 4:
				if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)ecapp_4_prod_acexsig;
					*cxiPubk = (u8*)ecapp_4_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
			case 5:
				if(ncchset->keys->keyset == pki_PRODUCTION){
					*accessDescSig = (u8*)ecapp_5_prod_acexsig;
					*cxiPubk = (u8*)ecapp_5_prod_hdrpub;
					*cxiPvtk = NULL;
				}
				break;
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == dlp){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 1:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)dlp_1_dev_acexsig;
					*cxiPubk = (u8*)dlp_1_dev_hdrpub;
					*cxiPvtk = (u8*)dlp_1_dev_hdrpvt;
				}
				break;
			case 2:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)dlp_2_dev_acexsig;
					*cxiPubk = (u8*)dlp_2_dev_hdrpub;
					*cxiPvtk = (u8*)dlp_2_dev_hdrpvt;
				}
				break;
			case 4:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)dlp_4_dev_acexsig;
					*cxiPubk = (u8*)dlp_4_dev_hdrpub;
					*cxiPvtk = (u8*)dlp_4_dev_hdrpvt;
				}
				break;
		}
	}
	else if(ncchset->keys->accessDescSign.presetType == demo){
		switch(ncchset->keys->accessDescSign.targetFirmware){
			case 2:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)demo_2_dev_acexsig;
					*cxiPubk = (u8*)demo_2_dev_hdrpub;
					*cxiPvtk = NULL;
				}
 				break;
			case 4:
				if(ncchset->keys->keyset == pki_DEVELOPMENT){
					*accessDescSig = (u8*)demo_4_dev_acexsig;
					*cxiPubk = (u8*)demo_4_dev_hdrpub;
					*cxiPvtk = (u8*)demo_4_dev_hdrpvt;
				}
 				break;
		}
	}
}
#endif

bool IsValidB64Char(char chr)
{
	return (isalnum(chr) || chr == '+' || chr == '/' || chr == '=');
}

u32 b64_strlen(char *str)
{
	u32 count = 0;
	u32 i = 0;
	while(str[i] != 0x0){
		if(IsValidB64Char(str[i])) {
			//printf("Is Valid: %c\n",str[i]);
			count++;
		}
		i++;
	}

	return count;
}

void b64_strcpy(char *dst, char *src)
{
	u32 src_len = strlen(src);
	u32 j = 0;
	for(u32 i = 0; i < src_len; i++){
		if(IsValidB64Char(src[i])){
			dst[j] = src[i];
			j++;
		}
	}
	dst[j] = 0;

	memdump(stdout,"src: ",(u8*)src,src_len+1);
	memdump(stdout,"dst: ",(u8*)dst,j+1);
}