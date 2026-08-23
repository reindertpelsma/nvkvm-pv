static void zero_nvos64_rights(unsigned job_type, unsigned job_nr,
			       void *param_buf, uint32_t param_size)
{
	static const uint64_t zero;

	if (job_type != 'F' || job_nr != 0x2b || !param_buf || param_size < 36)
		return;

	__builtin_memcpy((char *)param_buf + 24, &zero, sizeof(zero));
}
static void clamp_inner_params_size(unsigned job_type, unsigned job_nr,
				    void *param_buf, uint32_t param_size,
				    uint32_t aux_size)
{
	unsigned off;
	uint32_t declared;

	if (job_type != 'F' || !param_buf)
		return;
	if (job_nr == 0x2a && param_size >= 28)
		off = 24;
	else if (job_nr == 0x2b && param_size >= 36)
		off = 32;
	else
		return;

	__builtin_memcpy(&declared, (char *)param_buf + off, sizeof(declared));
	if (declared > aux_size)
		__builtin_memcpy((char *)param_buf + off, &aux_size,
				 sizeof(aux_size));
}
