#include "lib.h"
#include "ncch.h"
#include "exheader.h"
#include "elf.h"
#include "exefs.h"
#include "romfs.h"
#include "titleid.h"

#include "logo_data.h" // Contains Logos

// Private Prototypes
int SignCFA(u8 *Signature, u8 *CFA_HDR, keys_struct *keys);
int CheckCFASignature(u8 *Signature, u8 *CFA_HDR, keys_struct *keys);
int SignCXI(u8 *Signature, u8 *CXI_HDR, keys_struct *keys);
int CheckCXISignature(u8 *Signature, u8 *CXI_HDR, u8 *PubK);

void init_NCCHSettings(ncch_settings *set);
void free_NCCHSettings(ncch_settings *set);
int get_NCCHSettings(ncch_settings *ncchset, user_settings *usrset);
int SetBasicOptions(ncch_settings *ncchset, user_settings *usrset);
int CreateInputFilePtrs(ncch_settings *ncchset, user_settings *usrset);
int ImportNonCodeExeFsSections(ncch_settings *ncchset);	
int ImportLogo(ncch_settings *ncchset);

int SetCommonHeaderBasicData(ncch_settings *ncchset, ncch_hdr *hdr);
int SetCommonHeaderSectionData(ncch_settings *ncchset, ncch_hdr *hdr);
bool IsValidProductCode(char *ProductCode, bool FreeProductCode);

int BuildCommonHeader(ncch_settings *ncchset);
int EncryptNCCHSections(ncch_settings *ncchset);
int WriteNCCHSectionsToBuffer(ncch_settings *ncchset);

// Code

int SignCFA(u8 *Signature, u8 *CFA_HDR, keys_struct *keys)
{
	return ctr_sig(CFA_HDR,sizeof(ncch_hdr),Signature,keys->rsa.cciCfaPub,keys->rsa.cciCfaPvt,RSA_2048_SHA256,CTR_RSA_SIGN);
}

int CheckCFASignature(u8 *Signature, u8 *CFA_HDR, keys_struct *keys)
{
	return ctr_sig(CFA_HDR,sizeof(ncch_hdr),Signature,keys->rsa.cciCfaPub,NULL,RSA_2048_SHA256,CTR_RSA_VERIFY);
}

int SignCXI(u8 *Signature, u8 *CXI_HDR, keys_struct *keys)
{
	return ctr_sig(CXI_HDR,sizeof(ncch_hdr),Signature,keys->rsa.cxiHdrPub,keys->rsa.cxiHdrPvt,RSA_2048_SHA256,CTR_RSA_SIGN);
}

int CheckCXISignature(u8 *Signature, u8 *CXI_HDR, u8 *PubK)
{
	int result = ctr_sig(CXI_HDR,sizeof(ncch_hdr),Signature,PubK,NULL,RSA_2048_SHA256,CTR_RSA_VERIFY);
	return result;
}

// NCCH Build Functions

int build_NCCH(user_settings *usrset)
{
	int result;
#ifdef DEBUG
	printf("[DEBUG] Init Settings\n");
#endif
	// Init Settings
	ncch_settings *ncchset = malloc(sizeof(ncch_settings));
	if(!ncchset) {fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); return MEM_ERROR;}
	init_NCCHSettings(ncchset);

#ifdef DEBUG
	printf("[DEBUG] Get Settings\n");
#endif
	// Get Settings
	result = get_NCCHSettings(ncchset,usrset);
	if(result) goto finish;
#ifdef DEBUG
	printf("[DEBUG] Build ExeFS Code/PlainRegion\n");
#endif
	// Build ExeFs Code Section
	result = BuildExeFsCode(ncchset);
	if(result) goto finish;

#ifdef ELF_DEBUG
	FILE *code = fopen("code.bin","wb");
	fwrite(ncchset->exefsSections.code.buffer,ncchset->exefsSections.code.size,1,code);
	fclose(code);
	u8 hash[0x20];
	ctr_sha(ncchset->exefsSections.code.buffer,ncchset->exefsSections.code.size,hash,CTR_SHA_256);
	printf("BSS Size:  0x%x\n",ncchset->codeDetails.bssSize);
	printf("Code Size: 0x%x\n",ncchset->exefsSections.code.size);
	memdump(stdout,"Code Hash: ",hash,0x20);
#endif
	
#ifdef DEBUG
	printf("[DEBUG] Build Exheader\n");
#endif
	// Build ExHeader
	result = BuildExHeader(ncchset);
	if(result) goto finish;
	

#ifdef DEBUG
	printf("[DEBUG] Exefs\n");
#endif
	// Build ExeFs/RomFs
	result = BuildExeFs(ncchset);
	if(result) goto finish;
#ifdef DEBUG
	printf("[DEBUG] Build Romfs\n");
#endif
	result = BuildRomFs(ncchset);
	if(result) goto finish;
	
	// Final Steps
#ifdef DEBUG
	printf("[DEBUG] Build common header\n");
#endif
	result = BuildCommonHeader(ncchset);
	if(result) goto finish;
#ifdef DEBUG
	printf("[DEBUG] Encrypt Sections\n");
#endif
	result = EncryptNCCHSections(ncchset);
	if(result) goto finish;
#ifdef DEBUG
	printf("[DEBUG] Write Sections\n");
#endif
	result = WriteNCCHSectionsToBuffer(ncchset);
	if(result) goto finish;
finish:
#ifdef DEBUG
	printf("[DEBUG] Finish Building\n");
#endif
	if(result) fprintf(stderr,"[NCCH ERROR] NCCH Build Process Failed\n");
	free_NCCHSettings(ncchset);
	return result;
}

void init_NCCHSettings(ncch_settings *set)
{
	memset(set,0,sizeof(ncch_settings));
}

void free_NCCHSettings(ncch_settings *set)
{
	if(set->componentFilePtrs.elf) fclose(set->componentFilePtrs.elf);
	if(set->componentFilePtrs.banner) fclose(set->componentFilePtrs.banner);
	if(set->componentFilePtrs.icon) fclose(set->componentFilePtrs.icon);
	if(set->componentFilePtrs.logo) fclose(set->componentFilePtrs.logo);
	if(set->componentFilePtrs.code) fclose(set->componentFilePtrs.code);
	if(set->componentFilePtrs.exhdr) fclose(set->componentFilePtrs.exhdr);
	if(set->componentFilePtrs.romfs) fclose(set->componentFilePtrs.romfs);
	if(set->componentFilePtrs.plainregion) fclose(set->componentFilePtrs.plainregion);

	if(set->exefsSections.code.size) free(set->exefsSections.code.buffer);
	if(set->exefsSections.banner.size) free(set->exefsSections.banner.buffer);
	if(set->exefsSections.icon.size) free(set->exefsSections.icon.buffer);

	if(set->sections.ncchHdr.size) free(set->sections.ncchHdr.buffer);
	if(set->sections.exhdr.size) free(set->sections.exhdr.buffer);
	if(set->sections.logo.size) free(set->sections.logo.buffer);
	if(set->sections.plainRegion.size) free(set->sections.plainRegion.buffer);
	if(set->sections.exeFs.size) free(set->sections.exeFs.buffer);
	if(set->sections.romFs.size) free(set->sections.romFs.buffer);

	memset(set,0,sizeof(ncch_settings));

	free(set);
}

int get_NCCHSettings(ncch_settings *ncchset, user_settings *usrset)
{
	int result = 0;
	ncchset->out = &usrset->common.workingFile;
	ncchset->rsfSet = &usrset->common.rsfSet;
	ncchset->keys = &usrset->common.keys;

	result = SetBasicOptions(ncchset,usrset);
	if(result) return result;
	result = CreateInputFilePtrs(ncchset,usrset);
	if(result) return result;
	result = ImportNonCodeExeFsSections(ncchset);
	if(result) return result;
	result = ImportLogo(ncchset);
	if(result) return result;
	

	return 0;
}

int SetBasicOptions(ncch_settings *ncchset, user_settings *usrset)
{
	int result = 0;

	/* Options */
	ncchset->options.mediaSize = 0x200;

	ncchset->options.IncludeExeFsLogo = usrset->ncch.includeExefsLogo;
	
	if(usrset->common.rsfSet.Option.EnableCompress != -1) ncchset->options.CompressCode = usrset->common.rsfSet.Option.EnableCompress;
	else ncchset->options.CompressCode = true;

	if(usrset->common.rsfSet.Option.UseOnSD != -1) ncchset->options.UseOnSD = usrset->common.rsfSet.Option.UseOnSD;
	else ncchset->options.UseOnSD = false;
	usrset->common.rsfSet.Option.UseOnSD = ncchset->options.UseOnSD;

	if(usrset->common.rsfSet.Option.EnableCrypt != -1) ncchset->options.Encrypt = usrset->common.rsfSet.Option.EnableCrypt;
	else ncchset->options.Encrypt = true;

	if(usrset->common.rsfSet.Option.FreeProductCode != -1) ncchset->options.FreeProductCode = usrset->common.rsfSet.Option.FreeProductCode;
	else ncchset->options.FreeProductCode = false;

	ncchset->options.IsCfa = (usrset->ncch.ncchType == CFA);
	
	ncchset->options.IsBuildingCodeSection = (usrset->ncch.elfPath != NULL);

	ncchset->options.UseRomFS = ((ncchset->rsfSet->Rom.HostRoot && strlen(ncchset->rsfSet->Rom.HostRoot) > 0) || usrset->ncch.romfsPath);
	
	if(ncchset->options.IsCfa && !ncchset->options.UseRomFS){
		fprintf(stderr,"[NCCH ERROR] 'Rom/HostRoot' must be set\n");
		return NCCH_BAD_YAML_SET;
	}

	return result;
}

int CreateInputFilePtrs(ncch_settings *ncchset, user_settings *usrset)
{
	if(usrset->ncch.romfsPath){
		ncchset->componentFilePtrs.romfsSize = GetFileSize_u64(usrset->ncch.romfsPath);
		ncchset->componentFilePtrs.romfs = fopen(usrset->ncch.romfsPath,"rb");
		if(!ncchset->componentFilePtrs.romfs){
			fprintf(stderr,"[NCCH ERROR] Failed to open RomFs file '%s'\n",usrset->ncch.romfsPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.elfPath){
		ncchset->componentFilePtrs.elfSize = GetFileSize_u64(usrset->ncch.elfPath);
		ncchset->componentFilePtrs.elf = fopen(usrset->ncch.elfPath,"rb");
		if(!ncchset->componentFilePtrs.elf){
			fprintf(stderr,"[NCCH ERROR] Failed to open elf file '%s'\n",usrset->ncch.elfPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.bannerPath){
		ncchset->componentFilePtrs.bannerSize = GetFileSize_u64(usrset->ncch.bannerPath);
		ncchset->componentFilePtrs.banner = fopen(usrset->ncch.bannerPath,"rb");
		if(!ncchset->componentFilePtrs.banner){
			fprintf(stderr,"[NCCH ERROR] Failed to open banner file '%s'\n",usrset->ncch.bannerPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.iconPath){
		ncchset->componentFilePtrs.iconSize = GetFileSize_u64(usrset->ncch.iconPath);
		ncchset->componentFilePtrs.icon = fopen(usrset->ncch.iconPath,"rb");
		if(!ncchset->componentFilePtrs.icon){
			fprintf(stderr,"[NCCH ERROR] Failed to open icon file '%s'\n",usrset->ncch.iconPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.logoPath){
		ncchset->componentFilePtrs.logoSize = GetFileSize_u64(usrset->ncch.logoPath);
		ncchset->componentFilePtrs.logo = fopen(usrset->ncch.logoPath,"rb");
		if(!ncchset->componentFilePtrs.logo){
			fprintf(stderr,"[NCCH ERROR] Failed to open logo file '%s'\n",usrset->ncch.logoPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}

	if(usrset->ncch.codePath){
		ncchset->componentFilePtrs.codeSize = GetFileSize_u64(usrset->ncch.codePath);
		ncchset->componentFilePtrs.code = fopen(usrset->ncch.codePath,"rb");
		if(!ncchset->componentFilePtrs.code){
			fprintf(stderr,"[NCCH ERROR] Failed to open ExeFs Code file '%s'\n",usrset->ncch.codePath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.exheaderPath){
		ncchset->componentFilePtrs.exhdrSize = GetFileSize_u64(usrset->ncch.exheaderPath);
		ncchset->componentFilePtrs.exhdr = fopen(usrset->ncch.exheaderPath,"rb");
		if(!ncchset->componentFilePtrs.exhdr){
			fprintf(stderr,"[NCCH ERROR] Failed to open ExHeader file '%s'\n",usrset->ncch.exheaderPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	if(usrset->ncch.plainRegionPath){
		ncchset->componentFilePtrs.plainregionSize = GetFileSize_u64(usrset->ncch.plainRegionPath);
		ncchset->componentFilePtrs.plainregion = fopen(usrset->ncch.plainRegionPath,"rb");
		if(!ncchset->componentFilePtrs.plainregion){
			fprintf(stderr,"[NCCH ERROR] Failed to open PlainRegion file '%s'\n",usrset->ncch.plainRegionPath);
			return FAILED_TO_IMPORT_FILE;
		}
	}
	return 0;
}

int ImportNonCodeExeFsSections(ncch_settings *ncchset)
{
	if(ncchset->componentFilePtrs.banner){
		ncchset->exefsSections.banner.size = ncchset->componentFilePtrs.bannerSize;
		ncchset->exefsSections.banner.buffer = malloc(ncchset->exefsSections.banner.size);
		if(!ncchset->exefsSections.banner.buffer) {
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
			return MEM_ERROR;
		}
		ReadFile_64(ncchset->exefsSections.banner.buffer,ncchset->exefsSections.banner.size,0,ncchset->componentFilePtrs.banner);
	}
	if(ncchset->componentFilePtrs.icon){
		ncchset->exefsSections.icon.size = ncchset->componentFilePtrs.iconSize;
		ncchset->exefsSections.icon.buffer = malloc(ncchset->exefsSections.icon.size);
		if(!ncchset->exefsSections.icon.buffer) {
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
			return MEM_ERROR;
		}
		ReadFile_64(ncchset->exefsSections.icon.buffer,ncchset->exefsSections.icon.size,0,ncchset->componentFilePtrs.icon);
	}
	return 0;
}

int ImportLogo(ncch_settings *ncchset)
{
	if(ncchset->options.IsCfa) return 0;
	if(ncchset->componentFilePtrs.logo){
		ncchset->sections.logo.size = ncchset->componentFilePtrs.logoSize;
		ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
		if(!ncchset->sections.logo.buffer) {
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
			return MEM_ERROR;
		}
		ReadFile_64(ncchset->sections.logo.buffer,ncchset->sections.logo.size,0,ncchset->componentFilePtrs.logo);
	}
	else if(ncchset->rsfSet->BasicInfo.Logo){
		if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"nintendo") == 0){
			ncchset->sections.logo.size = 0x2000;
			ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
			if(!ncchset->sections.logo.buffer) {
				fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
				return MEM_ERROR;
			}
			memcpy(ncchset->sections.logo.buffer,Nintendo_LZ,0x2000);
		}
		else if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"licensed") == 0){
			ncchset->sections.logo.size = 0x2000;
			ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
			if(!ncchset->sections.logo.buffer) {
				fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
				return MEM_ERROR;
			}
			memcpy(ncchset->sections.logo.buffer,Nintendo_LicensedBy_LZ,0x2000);
		}
		else if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"distributed") == 0){
			ncchset->sections.logo.size = 0x2000;
			ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
			if(!ncchset->sections.logo.buffer) {
				fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
				return MEM_ERROR;
			}
			memcpy(ncchset->sections.logo.buffer,Nintendo_DistributedBy_LZ,0x2000);
		}
		else if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"ique") == 0){
			ncchset->sections.logo.size = 0x2000;
			ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
			if(!ncchset->sections.logo.buffer) {
				fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
				return MEM_ERROR;
			}
			memcpy(ncchset->sections.logo.buffer,iQue_with_ISBN_LZ,0x2000);
		}
		else if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"iqueforsystem") == 0){
			ncchset->sections.logo.size = 0x2000;
			ncchset->sections.logo.buffer = malloc(ncchset->sections.logo.size);
			if(!ncchset->sections.logo.buffer) {
				fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
				return MEM_ERROR;
			}
			memcpy(ncchset->sections.logo.buffer,iQue_without_ISBN_LZ,0x2000);
		}
		else if(strcasecmp(ncchset->rsfSet->BasicInfo.Logo,"none") != 0){
			fprintf(stderr,"[NCCH ERROR] Invalid logo name\n");
			return NCCH_BAD_YAML_SET;
		}
	}
	return 0;
}

int SetCommonHeaderBasicData(ncch_settings *ncchset, ncch_hdr *hdr)
{
	/* NCCH Format titleVersion */
	u16_to_u8(hdr->formatVersion,0x2,LE);
	
	/* Setting ProgramId/TitleId */
	u64 ProgramId = 0;
	int result = GetProgramID(&ProgramId,ncchset->rsfSet,false); 
	if(result) return result;

	u64_to_u8(hdr->programId,ProgramId,LE);
	u64_to_u8(hdr->titleId,ProgramId,LE);

	/* Get Product Code and Maker Code */
	if(ncchset->rsfSet->BasicInfo.ProductCode){
		if(!IsValidProductCode((char*)ncchset->rsfSet->BasicInfo.ProductCode,ncchset->options.FreeProductCode)){
			fprintf(stderr,"[NCCH ERROR] Invalid Product Code\n");
			return NCCH_BAD_YAML_SET;
		}
		memcpy(hdr->productCode,ncchset->rsfSet->BasicInfo.ProductCode,strlen((char*)ncchset->rsfSet->BasicInfo.ProductCode));
	}
	else memcpy(hdr->productCode,"CTR-P-CTAP",10);

	if(ncchset->rsfSet->BasicInfo.CompanyCode){
		if(strlen((char*)ncchset->rsfSet->BasicInfo.CompanyCode) != 2){
			fprintf(stderr,"[NCCH ERROR] Company code length must be 2\n");
			return NCCH_BAD_YAML_SET;
		}
		memcpy(hdr->makerCode,ncchset->rsfSet->BasicInfo.CompanyCode,2);
	}
	else memcpy(hdr->makerCode,"00",2);

	/* Set ContentUnitSize */
	hdr->flags[ContentUnitSize] = 0; // 0x200

	/* Setting ContentPlatform */
	hdr->flags[ContentPlatform] = 1; // CTR

	/* Setting OtherFlag */
	hdr->flags[OtherFlag] = FixedCryptoKey;
	if(!ncchset->options.Encrypt) hdr->flags[OtherFlag] |= NoCrypto;
	if(!ncchset->sections.romFs.size) hdr->flags[OtherFlag] |= NoMountRomFs;


	/* Setting ContentType */
	hdr->flags[ContentType] = 0;
	if(ncchset->sections.romFs.size || ncchset->options.IsCfa) hdr->flags[ContentType] |= content_Data;
	if(!ncchset->options.IsCfa) hdr->flags[ContentType] |= content_Executable;
	if(ncchset->rsfSet->BasicInfo.ContentType){
		if(strcmp(ncchset->rsfSet->BasicInfo.ContentType,"Application") == 0) hdr->flags[ContentType] |= 0;
		else if(strcmp(ncchset->rsfSet->BasicInfo.ContentType,"SystemUpdate") == 0) hdr->flags[ContentType] |= content_SystemUpdate;
		else if(strcmp(ncchset->rsfSet->BasicInfo.ContentType,"Manual") == 0) hdr->flags[ContentType] |= content_Manual;
		else if(strcmp(ncchset->rsfSet->BasicInfo.ContentType,"Child") == 0) hdr->flags[ContentType] |= content_Child;
		else if(strcmp(ncchset->rsfSet->BasicInfo.ContentType,"Trial") == 0) hdr->flags[ContentType] |= content_Trial;
		else{
			fprintf(stderr,"[NCCH ERROR] Invalid ContentType '%s'\n",ncchset->rsfSet->BasicInfo.ContentType);
			return NCCH_BAD_YAML_SET;
		}
	}

	return 0;
}

int SetCommonHeaderSectionData(ncch_settings *ncchset, ncch_hdr *hdr)
{
	/* Set Sizes/Hashes to Hdr */

	u32 ExHeaderSize,LogoSize,PlainRegionSize,ExeFsSize,ExeFsHashSize,RomFsSize,RomFsHashSize;
	
	ExHeaderSize = ncchset->sections.exhdr.size ? ((u32) ncchset->sections.exhdr.size - 0x400) : 0;
	LogoSize = ncchset->sections.logo.size ? ((u32) (ncchset->sections.logo.size/ncchset->options.mediaSize)) : 0;
	PlainRegionSize = ncchset->sections.plainRegion.size ? ((u32) (ncchset->sections.plainRegion.size/ncchset->options.mediaSize)) : 0;
	ExeFsSize = ncchset->sections.exeFs.size ? ((u32) (ncchset->sections.exeFs.size/ncchset->options.mediaSize)) : 0;
	ExeFsHashSize = (u32) ExeFsSize? ncchset->options.mediaSize/ncchset->options.mediaSize : 0;
	RomFsSize = ncchset->sections.romFs.size ? ((u32) (ncchset->sections.romFs.size/ncchset->options.mediaSize)) : 0;
	RomFsHashSize = (u32) RomFsSize? ncchset->options.mediaSize/ncchset->options.mediaSize : 0;
	

	u32_to_u8(hdr->exhdrSize,ExHeaderSize,LE);
	if(ExHeaderSize) ctr_sha(ncchset->sections.exhdr.buffer,ExHeaderSize,hdr->exhdrHash,CTR_SHA_256);

	u32_to_u8(hdr->logoSize,LogoSize,LE);
	if(LogoSize) ctr_sha(ncchset->sections.logo.buffer,ncchset->sections.logo.size,hdr->logoHash,CTR_SHA_256);

	u32_to_u8(hdr->plainRegionSize,PlainRegionSize,LE);

	u32_to_u8(hdr->exefsSize,ExeFsSize,LE);
	u32_to_u8(hdr->exefsHashSize,ExeFsHashSize,LE);
	if(ExeFsSize) ctr_sha(ncchset->sections.exeFs.buffer,ncchset->options.mediaSize,hdr->exefsHash,CTR_SHA_256);

	u32_to_u8(hdr->romfsSize,RomFsSize,LE);
	u32_to_u8(hdr->romfsHashSize,RomFsHashSize,LE);
	if(RomFsSize) ctr_sha(ncchset->sections.romFs.buffer,ncchset->options.mediaSize,hdr->romfsHash,CTR_SHA_256);


	/* Get Section Offsets */
	u32 size = 1;
	if (ExHeaderSize)
		size += 4;

	if (LogoSize){
		u32_to_u8(hdr->logoOffset,size,LE);
		ncchset->sections.logoOffset = size*ncchset->options.mediaSize;
		size += LogoSize;
	}

	if(PlainRegionSize){
		u32_to_u8(hdr->plainRegionOffset,size,LE);
		ncchset->sections.plainRegionOffset = size*ncchset->options.mediaSize;
		size += PlainRegionSize;
	}

	if (ExeFsSize){
		u32_to_u8(hdr->exefsOffset,size,LE);
		ncchset->sections.exeFsOffset = size*ncchset->options.mediaSize;
		size += ExeFsSize;
	}

	if (RomFsSize){
		u32_to_u8(hdr->romfsOffset,size,LE);
		ncchset->sections.romFsOffset = size*ncchset->options.mediaSize;
		size += RomFsSize;
	}

	u32_to_u8(hdr->ncchSize,size,LE);

	ncchset->sections.totalNcchSize = size * ncchset->options.mediaSize;

	return 0;
}

bool IsValidProductCode(char *ProductCode, bool FreeProductCode)
{
	if(strlen(ProductCode) > 16) return false;

	if(FreeProductCode)
		return true;

	if(strlen(ProductCode) < 10) return false;
	if(strncmp(ProductCode,"CTR-",4) != 0) return false;
	if(ProductCode[5] != '-') return false;
	if(!isdigit(ProductCode[4]) && !isupper(ProductCode[4])) return false;
	for(int i = 6; i < 10; i++){
		if(!isdigit(ProductCode[i]) && !isupper(ProductCode[i])) return false;
	}

	return true;
}

int BuildCommonHeader(ncch_settings *ncchset)
{
	int result = 0;

	// Initialising Header
	ncchset->sections.ncchHdr.size = 0x100 + sizeof(ncch_hdr);
	ncchset->sections.ncchHdr.buffer = malloc(ncchset->sections.ncchHdr.size);
	if(!ncchset->sections.ncchHdr.buffer) { fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); return MEM_ERROR; }
	memset(ncchset->sections.ncchHdr.buffer,0,ncchset->sections.ncchHdr.size);

	// Creating Ptrs
	u8 *sig = ncchset->sections.ncchHdr.buffer;
	ncch_hdr *hdr = (ncch_hdr*)(ncchset->sections.ncchHdr.buffer+0x100);

	// Setting Data in Hdr
	memcpy(hdr->magic,"NCCH",4);
	
	result = SetCommonHeaderBasicData(ncchset,hdr);
	if(result) return result;

	result = SetCommonHeaderSectionData(ncchset,hdr);
	if(result) return result;


	// Signing Hdr
	int sig_result = Good;
	if(ncchset->options.IsCfa) sig_result = SignCFA(sig,(u8*)hdr,ncchset->keys);
	else sig_result = SignCXI(sig,(u8*)hdr,ncchset->keys);
	if(sig_result != Good){
		fprintf(stderr,"[NCCH ERROR] Failed to sign %s header\n",ncchset->options.IsCfa ? "CFA" : "CXI");
		return sig_result;
	}

	return 0;
}

int EncryptNCCHSections(ncch_settings *ncchset)
{
	if(!ncchset->options.Encrypt) return 0;

	/* Getting ncch_struct */
	ncch_hdr *hdr = GetNCCH_CommonHDR(NULL,NULL,ncchset->sections.ncchHdr.buffer);
	ncch_struct *ncch = malloc(sizeof(ncch_struct));
	if(!ncch) { fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); return MEM_ERROR;}
	memset(ncch,0,sizeof(ncch_struct));
	GetCXIStruct(ncch,hdr);

	u8 *ncch_key = GetNCCHKey(hdr,ncchset->keys);

	if(ncchset->sections.exhdr.size)
		CryptNCCHSection(ncchset->sections.exhdr.buffer,ncchset->sections.exhdr.size,0,ncch,ncch_key,ncch_exhdr);

	if(ncchset->sections.exeFs.size)
		CryptNCCHSection(ncchset->sections.exeFs.buffer,ncchset->sections.exeFs.size,0,ncch,ncch_key,ncch_exefs);

	if(ncchset->sections.romFs.size)
		CryptNCCHSection(ncchset->sections.romFs.buffer,ncchset->sections.romFs.size,0,ncch,ncch_key,ncch_romfs);

	return 0;
}

int WriteNCCHSectionsToBuffer(ncch_settings *ncchset)
{
	/* Allocating Memory for NCCH, and clearing */
	ncchset->out->size = ncchset->sections.totalNcchSize;
	ncchset->out->buffer = malloc(ncchset->out->size);
	if(!ncchset->out->buffer) { fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); return MEM_ERROR;}
	memset(ncchset->out->buffer,0,ncchset->out->size);

	/* Copy Header+Sig */
	memcpy(ncchset->out->buffer,ncchset->sections.ncchHdr.buffer,ncchset->sections.ncchHdr.size);
	
	/* Copy Exheader+AccessDesc */
	if(ncchset->sections.exhdr.size)
		memcpy(ncchset->out->buffer+0x200,ncchset->sections.exhdr.buffer,ncchset->sections.exhdr.size);

	/* Copy Logo */
	if(ncchset->sections.logo.size)
		memcpy(ncchset->out->buffer+ncchset->sections.logoOffset,ncchset->sections.logo.buffer,ncchset->sections.logo.size);

	/* Copy PlainRegion */
	if(ncchset->sections.plainRegion.size)
		memcpy(ncchset->out->buffer+ncchset->sections.plainRegionOffset,ncchset->sections.plainRegion.buffer,ncchset->sections.plainRegion.size);

	/* Copy ExeFs */
	if(ncchset->sections.exeFs.size)
		memcpy(ncchset->out->buffer+ncchset->sections.exeFsOffset,ncchset->sections.exeFs.buffer,ncchset->sections.exeFs.size);

	/* Copy RomFs */
	if(ncchset->sections.romFs.size)
		memcpy(ncchset->out->buffer+ncchset->sections.romFsOffset,ncchset->sections.romFs.buffer,ncchset->sections.romFs.size);

	return 0;
}

// NCCH Read Functions

int VerifyNCCH(u8 *ncch, keys_struct *keys, bool SuppressOutput)
{
	// Setup
	u8 Hash[0x20];
	u8 *hdr_sig = ncch;
	ncch_hdr* hdr = GetNCCH_CommonHDR(NULL,NULL,ncch);

	ncch_struct *ncch_ctx = malloc(sizeof(ncch_struct));
	if(!ncch_ctx){ fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); return MEM_ERROR; }
	memset(ncch_ctx,0x0,sizeof(ncch_struct));
	GetCXIStruct(ncch_ctx,hdr);

	if(IsCfa(hdr)){
		if(CheckCFASignature(hdr_sig,(u8*)hdr,keys) != Good && !keys->rsa.isFalseSign){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CFA Sigcheck Failed\n");
			free(ncch_ctx);
			return NCCH_HDR_SIG_BAD;
		}
		if(!ncch_ctx->romfsSize){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CFA is corrupt\n");
			free(ncch_ctx);
			return NO_ROMFS_IN_CFA;
		}
	}
	else{ // IsCxi
		// Checking for necessary sections
		if(!ncch_ctx->exhdrSize){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CXI is corrupt\n");
			free(ncch_ctx);
			return NO_EXHEADER_IN_CXI;
		}
		if(!ncch_ctx->exefsSize){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CXI is corrupt\n");
			free(ncch_ctx);
			return NO_EXEFS_IN_CXI;
		}
		// Get ExHeader
		extended_hdr *ExHeader = malloc(ncch_ctx->exhdrSize);
		if(!ExHeader){ 
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
			free(ncch_ctx);
			return MEM_ERROR; 
		}
		int ret = GetNCCHSection((u8*)ExHeader,ncch_ctx->exhdrSize,0,ncch,ncch_ctx,keys,ncch_exhdr);
		if(ret != 0 && ret != UNABLE_TO_LOAD_NCCH_KEY){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CXI is corrupt\n");
			free(ncch_ctx);
			free(ExHeader);
			return CXI_CORRUPT;
		}
		else if(ret == UNABLE_TO_LOAD_NCCH_KEY){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] Failed to load ncch aes key.\n");
			free(ncch_ctx);
			free(ExHeader);
			return UNABLE_TO_LOAD_NCCH_KEY;
		}

		// Checking Exheader Hash to see if decryption was sucessful
		ctr_sha(ExHeader,0x400,Hash,CTR_SHA_256);
		if(memcmp(Hash,hdr->exhdrHash,0x20) != 0){
			//memdump(stdout,"Expected Hash: ",hdr->extended_header_sha_256_hash,0x20);
			//memdump(stdout,"Actual Hash:   ",Hash,0x20);
			//memdump(stdout,"Exheader:      ",(u8*)ExHeader,0x400);
			if(!SuppressOutput) {
				fprintf(stderr,"[NCCH ERROR] ExHeader Hashcheck Failed\n");
				fprintf(stderr,"[NCCH ERROR] CXI is corrupt\n");
			}
			free(ncch_ctx);
			free(ExHeader);
			return ExHeader_Hashfail;
		}

		// Checking RSA Sigs
		u8 *hdr_pubk = GetNcchHdrPubKey_frm_exhdr(ExHeader);

		if(CheckaccessDescSignature(ExHeader,keys) != 0 && !keys->rsa.isFalseSign){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] AccessDesc Sigcheck Failed\n");
			free(ncch_ctx);
			free(ExHeader);
			return ACCESSDESC_SIG_BAD;
		}
		if(CheckCXISignature(hdr_sig,(u8*)hdr,hdr_pubk) != 0 /* && !keys->rsa.isFalseSign*/){ // This should always be correct
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] CXI Header Sigcheck Failed\n");
			free(ncch_ctx);
			free(ExHeader);
			return NCCH_HDR_SIG_BAD;
		}
		free(ExHeader);
	}
	/* Checking ExeFs Hash, if present */
	if(ncch_ctx->exefsSize)
	{
		u8 *ExeFs = malloc(ncch_ctx->exefsHashDataSize);
		if(!ExeFs){ 
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
			free(ncch_ctx);
			return MEM_ERROR; 
		}
		GetNCCHSection(ExeFs,ncch_ctx->exefsHashDataSize,0,ncch,ncch_ctx,keys,ncch_exefs);
		ctr_sha(ExeFs,ncch_ctx->exefsHashDataSize,Hash,CTR_SHA_256);
		free(ExeFs);
		if(memcmp(Hash,hdr->exefsHash,0x20) != 0){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] ExeFs Hashcheck Failed\n");
			free(ncch_ctx);
			return ExeFs_Hashfail;
		}
	}

	/* Checking RomFs hash, if present */
	if(ncch_ctx->romfsSize){
		u8 *RomFs = malloc(ncch_ctx->romfsHashDataSize);
		if(!RomFs){ 
			fprintf(stderr,"[NCCH ERROR] MEM ERROR\n"); 
			free(ncch_ctx);
			return MEM_ERROR; 
		}
		GetNCCHSection(RomFs,ncch_ctx->romfsHashDataSize,0,ncch,ncch_ctx,keys,ncch_romfs);
		ctr_sha(RomFs,ncch_ctx->romfsHashDataSize,Hash,CTR_SHA_256);
		free(RomFs);
		if(memcmp(Hash,hdr->romfsHash,0x20) != 0){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] RomFs Hashcheck Failed\n");
			free(ncch_ctx);
			return ExeFs_Hashfail;
		}
	}

	/* Checking the Logo Hash, if present */
	if(ncch_ctx->logoSize){
		u8 *logo = (ncch+ncch_ctx->logoOffset);
		ctr_sha(logo,ncch_ctx->logoSize,Hash,CTR_SHA_256);
		if(memcmp(Hash,hdr->logoHash,0x20) != 0){
			if(!SuppressOutput) fprintf(stderr,"[NCCH ERROR] Logo Hashcheck Failed\n");
			free(ncch_ctx);
			return Logo_Hashfail;
		}
	} 
	
	
	free(ncch_ctx);
	return 0;
}


u8* RetargetNCCH(FILE *fp, u64 size, u8 *TitleId, u8 *ProgramId, keys_struct *keys)
{
	u8 *ncch = malloc(size);
	if(!ncch){
		fprintf(stderr,"[NCCH ERROR] MEM ERROR\n");
		return NULL;
	}
	ReadFile_64(ncch,size,0,fp); // Importing
	
	if(!IsNCCH(NULL,ncch)){
		free(ncch);
		return NULL;
	}
		
	ncch_hdr *hdr = NULL;
	hdr = GetNCCH_CommonHDR(NULL,NULL,ncch);
	
	if(/*keys->rsa.requiresPresignedDesc && */!IsCfa(hdr)){
		fprintf(stderr,"[NCCH ERROR] CXI's ID cannot be modified without the ability to resign the AccessDesc\n"); // Not yet yet, requires AccessDesc Privk, may implement anyway later
		free(ncch);
		return NULL;
	}
	
	if((memcmp(TitleId,hdr->titleId,8) == 0) && (memcmp(ProgramId,hdr->programId,8) == 0)) 
		return ncch;// if no modification is required don't do anything

	if(memcmp(TitleId,hdr->titleId,8) == 0){ // If TitleID Same, no crypto required, just resign.
		memcpy(hdr->programId,ProgramId,8);
		SignCFA(ncch,(u8*)hdr,keys);
		return ncch;
	}

	ncch_key_type keytype = GetNCCHKeyType(hdr);
	u8 *key = NULL;
	
	if(keytype == KeyIsUnFixed || keytype == KeyIsUnFixed2){
		fprintf(stderr,"[NCCH ERROR] Unknown aes key\n");
		free(ncch);
		return NULL;
	}
	
	
	ncch_struct ncch_struct;
	if(keytype != NoKey){ //Decrypting if necessary
		GetCXIStruct(&ncch_struct,hdr);
		u8 *romfs = (ncch+ncch_struct.romfsOffset);
		key = GetNCCHKey(hdr,keys);
		if(key == NULL){
			fprintf(stderr,"[NCCH ERROR] Failed to load ncch aes key\n");
			free(ncch);
			return NULL;
		}
		CryptNCCHSection(romfs,ncch_struct.romfsSize,0,&ncch_struct,key,ncch_romfs);
	}
	
	
	memcpy(hdr->titleId,TitleId,8);
	memcpy(hdr->programId,ProgramId,8);
	
	//Checking New Fixed Key Type
	keytype = GetNCCHKeyType(hdr);
	
	if(keytype != NoKey){ // Re-encrypting if necessary
		GetCXIStruct(&ncch_struct,hdr);
		u8 *romfs = (ncch+ncch_struct.romfsOffset);
		key = GetNCCHKey(hdr,keys);
		if(key == NULL){
			fprintf(stderr,"[NCCH ERROR] Failed to load ncch aes key\n");
			free(ncch);
			return NULL;
		}
		CryptNCCHSection(romfs,ncch_struct.romfsSize,0,&ncch_struct,key,ncch_romfs);
	}
	
	SignCFA(ncch,(u8*)hdr,keys);
	
	return ncch;
}


ncch_hdr* GetNCCH_CommonHDR(void *out, FILE *fp, u8 *buf)
{
	if(!fp && !buf) return NULL;
	if(fp){
		if(!out) return NULL;
		ReadFile_64(out,0x100,0x100,fp);
		return (ncch_hdr*)out;
	}
	else{
		return (ncch_hdr*)(buf+0x100);
	}
}


bool IsNCCH(FILE *fp, u8 *buf)
{
	if(!fp && !buf) return false;
	ncch_hdr *ncchHDR = NULL;
	bool result;
	if(fp) {
		ncchHDR = malloc(sizeof(ncch_hdr));
		GetNCCH_CommonHDR(ncchHDR,fp,NULL);
		result = (memcmp(ncchHDR->magic,"NCCH",4) == 0);
		free(ncchHDR);
	}
	else {
		ncchHDR = GetNCCH_CommonHDR(ncchHDR,NULL,buf);
		result = (memcmp(ncchHDR->magic,"NCCH",4) == 0);
	}
	return result;
}

bool IsCfa(ncch_hdr* hdr)
{
	return (((hdr->flags[ContentType] & content_Data) == content_Data) && ((hdr->flags[ContentType] & content_Executable) != content_Executable));
}

u32 GetNCCH_MediaUnitSize(ncch_hdr* hdr)
{
	u16 titleVersion = u8_to_u16(hdr->formatVersion,LE);
	u32 ret = 0;
	if (titleVersion == 1)
		ret = 1;
	else if (titleVersion == 2 || titleVersion == 0)
		ret = 1 << (hdr->flags[ContentUnitSize] + 9);
	return ret;
	//return 0x200*pow(2,hdr->flags[ContentUnitSize]);
}

u32 GetNCCH_MediaSize(ncch_hdr* hdr)
{
	return u8_to_u32(hdr->ncchSize,LE);
}

ncch_key_type GetNCCHKeyType(ncch_hdr* hdr)
{	
	// Non-Secure Key Options
	if((hdr->flags[OtherFlag] & NoCrypto) == NoCrypto) 
		return NoKey;
	if((hdr->flags[OtherFlag] & FixedCryptoKey) == FixedCryptoKey){
		if((hdr->programId[4] & 0x10) == 0x10) 
			return KeyIsSystemFixed;
		else 
			return KeyIsNormalFixed;
	}

	// Secure Key Options
	if(hdr->flags[SecureCrypto2]) 
		return KeyIsUnFixed2;
	return KeyIsUnFixed;
}

u8* GetNCCHKey(ncch_hdr* hdr, keys_struct *keys)
{
	ncch_key_type keytype = GetNCCHKeyType(hdr);
	switch(keytype){
		case NoKey: return NULL;
		case KeyIsNormalFixed: return keys->aes.normalKey;
		case KeyIsSystemFixed:
			if(!keys->aes.systemFixedKey) fprintf(stderr,"[NCCH WARNING] Unable to load SystemFixed Key\n");
			return keys->aes.systemFixedKey;
		case KeyIsUnFixed:
			fprintf(stderr,"[NCCH WARNING] Unable to load UnFixed Key\n");
			return NULL;
			//if(!keys->aes.unFixedKey0) fprintf(stderr,"[NCCH WARNING] Unable to load UnFixed Key\n");
			//return keys->aes.unFixedKey0;
		case KeyIsUnFixed2:
			fprintf(stderr,"[NCCH WARNING] Crypto method (Secure2) not supported yet\n");
			return NULL;
	}
	return NULL;
}

int GetNCCHSection(u8 *dest, u64 dest_max_size, u64 src_pos, u8 *ncch, ncch_struct *ncch_ctx, keys_struct *keys, ncch_section section)
{
	if(!ncch) return MEM_ERROR;
	u8 *key = NULL;
	ncch_hdr* hdr = GetNCCH_CommonHDR(NULL,NULL,ncch);
	ncch_key_type keytype = GetNCCHKeyType(hdr);

	if(keytype != NoKey && (section == ncch_exhdr || section == ncch_exefs || section == ncch_romfs)){
		key = GetNCCHKey(hdr,keys);
		if(key == NULL){
			//fprintf(stderr,"[NCCH ERROR] Failed to load ncch aes key.\n");
			return UNABLE_TO_LOAD_NCCH_KEY;
		}
	}
	//printf("detecting section type\n");
	u64 offset = 0;
	u64 size = 0;
	switch(section){
		case ncch_exhdr:
			offset = ncch_ctx->exhdrOffset;
			size = ncch_ctx->exhdrSize;
			break;
		case ncch_Logo:
			offset = ncch_ctx->logoOffset;
			size = ncch_ctx->logoSize;
			break;
		case ncch_PlainRegion:
			offset = ncch_ctx->plainRegionOffset;
			size = ncch_ctx->plainRegionSize;
			break;
		case ncch_exefs:
			offset = ncch_ctx->exefsOffset;
			size = ncch_ctx->exefsSize;
			break;
		case ncch_romfs:
			offset = ncch_ctx->romfsOffset;
			size = ncch_ctx->romfsSize;
			break;
	}
	if(!offset || !size) return NCCH_SECTION_NOT_EXIST; 

	if(src_pos > size) return DATA_POS_DNE;

	size = min_u64(size-src_pos,dest_max_size);

	//printf("Copying data\n");
	u8 *section_pos = (ncch + offset + src_pos);
	memcpy(dest,section_pos,size);

	//printf("decrypting if needed\n");
	if(keytype != NoKey && (section == ncch_exhdr || section == ncch_exefs || section == ncch_romfs)){ // Decrypt
		//memdump(stdout,"Key: ",key,16);
		CryptNCCHSection(dest,size,src_pos,ncch_ctx,key,section);
		//printf("no cigar\n");
	}
	//printf("Got thing okay\n");
	return 0;
}

int GetCXIStruct(ncch_struct *ctx, ncch_hdr *header)
{
	memcpy(ctx->titleId,header->titleId,8);
	memcpy(ctx->programId,header->programId,8);

	
	u32 media_unit = GetNCCH_MediaUnitSize(header);
	
	ctx->formatVersion = u8_to_u16(header->formatVersion,LE);
	if(!IsCfa(header)){
		ctx->exhdrOffset = 0x200;
		ctx->exhdrSize = u8_to_u32(header->exhdrSize,LE) + 0x400;
		ctx->plainRegionOffset = (u64)(u8_to_u32(header->plainRegionOffset,LE)*media_unit);
		ctx->plainRegionSize = (u64)(u8_to_u32(header->plainRegionSize,LE)*media_unit);
	}

	ctx->logoOffset = (u64)(u8_to_u32(header->logoOffset,LE)*media_unit);
	ctx->logoSize = (u64)(u8_to_u32(header->logoSize,LE)*media_unit);
	ctx->exefsOffset = (u64)(u8_to_u32(header->exefsOffset,LE)*media_unit);
	ctx->exefsSize = (u64)(u8_to_u32(header->exefsSize,LE)*media_unit);
	ctx->exefsHashDataSize = (u64)(u8_to_u32(header->exefsHashSize,LE)*media_unit);
	ctx->romfsOffset = (u64) (u8_to_u32(header->romfsOffset,LE)*media_unit);
	ctx->romfsSize = (u64) (u8_to_u32(header->romfsSize,LE)*media_unit);
	ctx->romfsHashDataSize = (u64)(u8_to_u32(header->romfsHashSize,LE)*media_unit);
	return 0;
}

void CryptNCCHSection(u8 *buffer, u64 size, u64 src_pos, ncch_struct *ctx, u8 key[16], u8 type)
{
	if(type < 1 || type > 3)
		return;
	u8 counter[0x10];
	ncch_get_counter(ctx,counter,type);	
	ctr_aes_context aes_ctx;
	memset(&aes_ctx,0x0,sizeof(ctr_aes_context));
	ctr_init_counter(&aes_ctx, key, counter);
	if(src_pos > 0){
		u32 carry = 0;
		carry = align_value(src_pos,0x10);
		carry /= 0x10;
		ctr_add_counter(&aes_ctx,carry);
	}
	ctr_crypt_counter(&aes_ctx, buffer, buffer, size);
	return;
}

void ncch_get_counter(ncch_struct *ctx, u8 counter[16], u8 type)
{
	u8 *titleId = ctx->titleId;
	u32 i;
	u32 x = 0;

	memset(counter, 0, 16);

	if (ctx->formatVersion == 2 || ctx->formatVersion == 0)
	{
		for(i=0; i<8; i++)
			counter[i] = titleId[7-i];
		counter[8] = type;
	}
	else if (ctx->formatVersion == 1)
	{
		switch(type){
			case ncch_exhdr : x = ctx->exhdrOffset; break;
			case ncch_exefs : x = ctx->exefsOffset; break;
			case ncch_romfs : x = ctx->romfsOffset; break;
		}
		for(i=0; i<8; i++)
			counter[i] = titleId[i];
		for(i=0; i<4; i++)
			counter[12+i] = x>>((3-i)*8);
	}
	
	//memdump(stdout,"CTR: ",counter,16);
}