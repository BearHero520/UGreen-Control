FROM node:24-bookworm-slim AS frontend
WORKDIR /build
COPY web-ui/package*.json ./web-ui/
RUN cd web-ui && npm ci
COPY web-ui ./web-ui
COPY synology/ui/UGreenToolbox.js ./synology/ui/UGreenToolbox.js
COPY app/www/css/style.css ./app/www/css/style.css
RUN cd web-ui && npm run check && npm test && npm run build

FROM python:3.11-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash coreutils findutils grep sed gawk util-linux procps iproute2 hdparm smartmontools \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/ugreen
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
COPY app/server ./app/server
COPY app/ui ./app/ui
COPY app/app-info.json ./app/app-info.json
COPY app/www/images ./app/www/images
COPY app/www/vendor ./app/www/vendor
COPY README.md ./app/README.md
COPY LICENSE ./LICENSE
COPY --from=frontend /build/app/www ./app/www
RUN chmod 0755 app/server/bin/* \
    && python3 app/server/portable_runtime.py verify
USER 568:568
EXPOSE 5088
ENTRYPOINT ["python3", "/opt/ugreen/app/server/portable_runtime.py"]
CMD ["web"]
