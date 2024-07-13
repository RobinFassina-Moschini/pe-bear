#include "CDisasm.h"
#include <iostream>
using namespace pe_bear;

//const int CDisasm::MAX_ARG_NUM = 2;

CDisasm::CDisasm()
	: Disasm(),
	m_insn(NULL)
{
}

CDisasm::~CDisasm()
{
	is_init = false;
}

cs_mode toCSmode(Executable::exe_bits bitMode)
{
	switch (bitMode) {
	case Executable::BITS_16:
		return CS_MODE_16;
	case Executable::BITS_32:
		return CS_MODE_32;
	case Executable::BITS_64:
		return CS_MODE_64;
	}
	return CS_MODE_32; //Default
}

bool CDisasm::init_capstone(Executable::exe_arch arch, Executable::exe_bits bitMode)
{
	cs_err err;
	if (arch == Executable::ARCH_INTEL) {
		err = cs_open(CS_ARCH_X86, toCSmode(bitMode), &handle);
	} else if (arch == Executable::ARCH_ARM && bitMode == Executable::BITS_64) {
		err = cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle);
	} else if (arch == Executable::ARCH_ARM && bitMode == Executable::BITS_32) {
		err = cs_open(CS_ARCH_ARM, CS_MODE_LITTLE_ENDIAN, &handle);
	} else {
		std::cout << "Unknown ARCH: " << std::hex << arch << "\n";
		return false;
	}

	if (err) {
		if (err == CS_ERR_ARCH) {
			std::cerr << "Failed on cs_open(): unsupported architecture supplied!\n";
		} else {
			std::cerr << "Failed on cs_open(), error: " << std::dec << err << std::endl;
		}
		return false;
	}
	cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
	cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);
	m_insn = cs_malloc(handle);
	if (!m_insn) {
		cs_close(&handle);
		return false;
	}
	return true;
}

bool CDisasm::init(uint8_t* buf, size_t bufSize, size_t disasmSize, offset_t offset, Executable::exe_arch arch, Executable::exe_bits bitMode)
{
	QMutexLocker locker(&m_disasmMutex);
	is_init = false;
	if (!buf || bufSize == 0) return false;

	m_buf = buf;
	m_bufSize = bufSize;
	m_disasmSize = disasmSize;
	m_iptr = 0;

	this->m_offset = 0;
	this->startOffset = this->convertToVA(offset);
	this->m_RVA = (this->startOffset != INVALID_ADDR) ? this->startOffset : 0;
	m_bitMode = bitMode;
	m_arch = arch;

	is_init = init_capstone(m_arch, m_bitMode);
	return this->is_init;
}

size_t CDisasm::disasmNext()
{
	if (!is_init && m_insn) {
		printf("Cannot disasm next = not initialized!\n");
		return 0;
	}
	//--
	bool isOk = cs_disasm_iter(handle, (const unsigned char**)&m_buf, &m_bufSize, &m_RVA, m_insn);
	if (!isOk || !m_insn) {
		is_init = false;
		return 0;
	}
	//--
	const size_t step = m_insn->size;
	m_iptr += step;
	return step;
}

bool CDisasm::fillTable()
{
	QMutexLocker locker(&m_disasmMutex);

	if (!is_init) {
		return false;
	}
	this->clearTable();
	size_t processedSize = 0;
	while (processedSize < this->m_disasmSize) {
		if (!disasmNext()) {
			break; //could not disasemble more
		}
		if (!m_insn) continue;
		processedSize += m_insn->size;

		const cs_insn next_insn = *m_insn;
		const cs_detail *detail = m_insn->detail;
		m_table.push_back(next_insn);
		m_details.push_back(*detail);
	}
	if (m_table.size() == 0) {
		return false;
	}
	return true;
}

bool CDisasm::clearTable()
{
	m_table.clear();
	m_details.clear();
	return true;
}

offset_t CDisasm::getRawAt(int index) const
{
	if (index >= m_table.size()) {
		return INVALID_ADDR;
	}
	const cs_insn m_insn = m_table.at(index);
	if (startOffset == INVALID_ADDR) {
		return m_insn.address;
	}
	return m_insn.address - startOffset;
}

offset_t CDisasm::getArgVA(int index, int argNum, bool &isOk) const
{
	isOk = false;
	if (index >= m_table.size()) {
		return INVALID_ADDR;
	}
	const cs_insn m_insn = m_table.at(index);
	const cs_detail *m_detail = &m_details.at(index);
	
	offset_t va = INVALID_ADDR;
	// Intel
	if (this->m_arch == Executable::ARCH_INTEL) {
		size_t cnt = static_cast<size_t>(m_detail->x86.op_count);
		if (argNum >= cnt) return INVALID_ADDR;
		
		const x86_op_type op_type = m_detail->x86.operands[argNum].type;

		if (op_type == X86_OP_MEM) {
			int64_t lval = m_detail->x86.operands[argNum].mem.disp;
			const x86_reg reg = static_cast<x86_reg>(m_detail->x86.operands[argNum].mem.base);
			const bool isEIPrelative = (reg == X86_REG_IP || reg == X86_REG_EIP || reg == X86_REG_RIP);
			if (isEIPrelative) {
				const offset_t currVA = getVaAt(index);
				const size_t instrLen = getChunkSize(index);
				va = Disasm::getJmpDestAddr(currVA, instrLen, lval);
			}
			else if (reg <= X86_REG_INVALID) { //simple case, no reg value to add
				va = Disasm::trimToBitMode(lval, this->m_bitMode);
			}
		}
		if (op_type == X86_OP_IMM) {
			va = m_detail->x86.operands[argNum].imm;
		}
	}
	//ARM:
	else if (this->m_arch == Executable::ARCH_ARM && this->m_bitMode == 64) {
		size_t cnt = static_cast<size_t>(m_detail->arm64.op_count);
		if (argNum >= cnt) return INVALID_ADDR;
		
		//immediate:
		if (m_detail->arm64.operands[argNum].type == ARM64_OP_IMM) {
			va = m_detail->arm64.operands[argNum].imm;
		}
	}
	// cleanup
	if (va != INVALID_ADDR) {
		isOk = true;
		va = Disasm::trimToBitMode(va, this->m_bitMode);
	}
	return va;
}

minidis::mnem_type CDisasm::fetchMnemType_Intel(const cs_insn &insn) const
{
	using namespace minidis;

	const unsigned int cMnem = insn.id;
	if (cMnem == x86_insn::X86_INS_INVALID) {
		return MT_INVALID;
	}
	if (cMnem == x86_insn::X86_INS_JMP || cMnem == x86_insn::X86_INS_LJMP) {
		return MT_JUMP;
	}
	if (cMnem >= x86_insn::X86_INS_JAE && cMnem <= x86_insn::X86_INS_JS) {
		return MT_COND_JUMP;
	}
	if (cMnem >= x86_insn::X86_INS_MOV && cMnem <= x86_insn::X86_INS_MOVZX) {
		return MT_MOV;
	}

	switch (cMnem) {
	case X86_INS_LOOP:
	case X86_INS_LOOPE:
	case X86_INS_LOOPNE:
		return MT_LOOP;

	case X86_INS_CALL:
	case X86_INS_LCALL:
		return MT_CALL;

	case X86_INS_RET:
	case X86_INS_RETF:
	case X86_INS_RETFQ:
		return MT_RET;

	case X86_INS_NOP: return MT_NOP;

	case X86_INS_POP:
	case X86_INS_POPAW:
	case X86_INS_POPAL:
	case X86_INS_POPCNT:
	case X86_INS_POPF:
	case X86_INS_POPFD:
	case X86_INS_POPFQ:
	{
		return MT_POP;
	}
	case X86_INS_PUSH:
	case X86_INS_PUSHAW:
	case X86_INS_PUSHAL:
	case X86_INS_PUSHF:
	case X86_INS_PUSHFD:
	case X86_INS_PUSHFQ:
	{
		return MT_PUSH;
	}
	case X86_INS_INT3:
		return MT_INT3;

	case X86_INS_INT:
		return MT_INTX;
	}
	return MT_OTHER;
}

minidis::mnem_type CDisasm::fetchMnemType_Arm64(const cs_insn &insn, const cs_detail &detail) const
{
	using namespace minidis;

	const unsigned int cMnem = insn.id;
	if (cMnem == arm64_insn::ARM64_INS_UDF) {
		return MT_INT3;
	}
	if (cMnem == arm64_insn::ARM64_INS_INVALID) {
		return MT_INVALID;
	}
	if (cMnem == arm64_insn::ARM64_INS_NOP) {
		return MT_NOP;
	}
	for (size_t i = 0; i < detail.groups_count; i++) {
		if (detail.groups[i] == ARM64_GRP_CALL) return MT_CALL;
		if (detail.groups[i] == ARM64_GRP_RET) return MT_RET;
		if (detail.groups[i] == ARM64_GRP_INT)  return MT_INTX;
		
		if (detail.groups[i] == ARM64_GRP_JUMP || detail.groups[i] == ARM64_GRP_BRANCH_RELATIVE) {
			switch (cMnem) {
				case arm64_insn::ARM64_INS_CBZ:
				case arm64_insn::ARM64_INS_CBNZ:
				case arm64_insn::ARM64_INS_TBNZ:
				case arm64_insn::ARM64_INS_TBZ:
					return MT_COND_JUMP;
			}
			return MT_JUMP;
		}

	}
	return MT_OTHER;
}

bool CDisasm::isPushRet(int index, /*out*/ int* ret_index) const
{
	if (index >= this->_chunksCount()) {
		return false;
	}
	
	const cs_insn m_insn = m_table.at(index);
	const cs_detail detail = m_details.at(index);

	const minidis::mnem_type mnem = fetchMnemType(m_insn, detail);
	if (mnem == minidis::MT_PUSH) {
		int y2 = index + 1;
		if (y2 >= m_table.size()) {
			return false;
		}
		const cs_insn m_insn2 = m_table.at(y2);
		const minidis::mnem_type mnem2 = fetchMnemType(m_insn2, detail);
		if (mnem2 == minidis::MT_RET) {
			if (ret_index != NULL) {
				(*ret_index) = y2;
			}
			return true;
		}
	}
	return false;
}

bool CDisasm::isAddrOperand(int index) const
{
	if (index >= m_table.size()) {
		return false;
	}
	using namespace minidis;
	mnem_type mnem = this->getMnemType(index);
	if (mnem == MT_PUSH || mnem == MT_MOV) return true;

	const cs_detail *detail = &m_details.at(index);
	const size_t cnt = static_cast<size_t>(detail->x86.op_count);

	for (int argNum = 0; argNum < cnt; argNum++) {
		const x86_op_type type = detail->x86.operands[argNum].type;
		const size_t opSize = detail->x86.operands[argNum].size;

		const x86_reg reg = static_cast<x86_reg>(detail->x86.operands[argNum].mem.base);
		const bool isEIPrelative = (reg == X86_REG_IP || reg == X86_REG_EIP || reg == X86_REG_RIP);

		if (type == X86_OP_IMM
			&& opSize > 8)
		{
			return true;
		}
		if (type == X86_OP_MEM && isEIPrelative) {
			return true;
		}
	}
	return false;
}

bool CDisasm::isFollowable(const int y) const
{
	if (y >= this->chunksCount()) return false;

	if (getRvaAt(y) == INVALID_ADDR) return false;

	if (!isBranching(y) && !isPushRet(y)) {
		return false;
	}
	const cs_detail *detail = &m_details.at(y);
	if (!detail) return false;

	const size_t argNum = 0;
	// Intel
	if (this->m_arch == Executable::ARCH_INTEL) {
		size_t cnt = static_cast<size_t>(detail->x86.op_count);
		if (!cnt) {
			return false;
		}
		const x86_op_type op_type = detail->x86.operands[argNum].type;
		if (op_type == X86_OP_IMM) {
			return true;
		}
		if (op_type == X86_OP_MEM || op_type == X86_OP_IMM) {
			const x86_reg reg = static_cast<x86_reg>(detail->x86.operands[argNum].mem.base);
			if (reg <= X86_REG_INVALID) { //simple case, no reg value to add
				return true;
			}
			const bool isEIPrelative = (reg == X86_REG_IP || reg == X86_REG_EIP || reg == X86_REG_RIP);
			if (isEIPrelative) {
				return true;
			}
		}
		return false;
	}
	// ARM
	else if (this->m_arch == Executable::ARCH_ARM && this->m_bitMode == 64) {
		size_t cnt = static_cast<size_t>(detail->arm64.op_count);
		if (!cnt) {
			return false;
		}
		if (detail->arm64.operands[argNum].type == ARM64_OP_IMM) {
			return true;
		}
	}
	return false;
}
