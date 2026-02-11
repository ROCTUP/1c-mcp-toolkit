FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY onec_mcp_toolkit_proxy/ ./onec_mcp_toolkit_proxy/

ENV PORT=6003
ENV TIMEOUT=180
ENV LOG_LEVEL=INFO

EXPOSE 6003

CMD ["python", "-m", "onec_mcp_toolkit_proxy"]
